// SPDX-License-Identifier: Apache-2.0
// シャドウマップ（有向光源用、orthographic）
//
// 2 つのモード:
//   1. Single cascade (既定、cascade_count=1): 1 つの 2D 深度テクスチャに
//      シーン全体を 1 つの ortho 投影で描く。HelloShadows (StandardShader) や
//      昔の HelloIbl が使う伝統的な方式。
//   2. Cascaded Shadow Map (CSM、cascade_count >= 2): カメラ frustum を
//      距離で 2-4 個に分割し、近景は高解像度・遠景は広範囲を 1 枚の atlas
//      テクスチャ (width = cascade_count * size、height = size) に並べる。
//      ピクセルの view-space z で cascade を選択するため遠景まで鮮鋭な影を
//      保てる (UE5 等 large outdoor scene の標準解)。
//
// 使い方 (single cascade、後方互換):
//   ShadowMap sm;
//   sm.Init(*dev, /*size=*/2048);                    // cascade_count=1 既定
//   sm.SetDirectionalLight(light_dir, scene_center, 15.0f);
//   cl->BeginShadowPass(*sm.DepthTexture(), 1.0f);
//   cl->SetPipeline(*sm.CasterPipeline());
//   cl->SetConstantBuffer(0, *sm.LightCB());
//   for (each caster) { sm.SetCaster(model); /* draw */ }
//   cl->EndShadowPass(*sm.DepthTexture());
//
// 使い方 (CSM、3 cascade):
//   ShadowMap sm;
//   sm.Init(*dev, 2048, /*cascade_count=*/3);
//   sm.SetDirectionalLightCascades(light_dir, view, proj, 0.1f, 100.0f);
//   cl->BeginShadowPass(*sm.DepthTexture(), 1.0f);        // atlas 全体 clear
//   cl->SetPipeline(*sm.CasterPipeline());
//   for (u32 c = 0; c < sm.CascadeCount(); ++c) {
//       cl->SetViewport(sm.CascadeViewport(c));
//       cl->SetScissor(sm.CascadeScissor(c));
//       sm.SetCurrentCascade(c);
//       cl->SetConstantBuffer(0, *sm.LightCB());
//       for (each caster) { sm.SetCaster(model); /* draw */ }
//   }
//   cl->EndShadowPass(*sm.DepthTexture());
//
// 主パスでの使用 (PbrShader 統合):
//   single mode: pbr.SetShadowMap(*sm.DepthTexture(), sm.LightViewProjection(), ...);
//   CSM mode   : FMat4 vps[3] = { sm.LightViewProjection(0), sm.LightViewProjection(1), sm.LightViewProjection(2) };
//                f32  spl[3] = { sm.CascadeSplit(0), sm.CascadeSplit(1), sm.CascadeSplit(2) };
//                pbr.SetShadowMapCascades(*sm.DepthTexture(), vps, spl, sm.CascadeCount(), ...);
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/RhiTypes.h"        // Viewport / ScissorRect

namespace acs {

class ShadowMap {
public:
    static constexpr u32 kMaxCascades = 4;

    ShadowMap() noexcept = default;
    ~ShadowMap() noexcept = default;

    ShadowMap(const ShadowMap&)            = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    // cascade_count = 1 (既定): single 2D depth texture (size × size)
    // cascade_count >= 2     : CSM atlas (cascade_count*size × size)
    // cascade_count > kMaxCascades は kMaxCascades にクランプ
    TResult<void> Init(IRhiDevice& device, u32 size = 2048,
                      u32 cascade_count = 1) noexcept;
    void Shutdown() noexcept;

    // single cascade 用 (cascade_count=1 でのみ意味を持つ、後方互換)。
    // CSM mode では SetDirectionalLightCascades を使うこと。
    void SetDirectionalLight(FVec3 light_dir,
                              FVec3 scene_center,
                              f32 scene_radius) noexcept;

    // CSM 用: カメラ frustum を near→far で 2-4 個に「実用分割」(Zhang) して
    // 各 sub-frustum を bounding sphere で囲い ortho 投影を計算。
    //   lambda: 0 = uniform split (近景密、線形)、1 = log split (遠景均等)
    //   既定 0.5 は両者のブレンド (実用 sweet spot)
    void SetDirectionalLightCascades(FVec3 light_dir,
                                      const FMat4& view, const FMat4& proj,
                                      f32 near_z, f32 far_z,
                                      f32 lambda = 0.5f) noexcept;

    // 現在描画する cascade を選択 (CSM mode、BeginShadowPass の後に呼ぶ)。
    // _light_cb を当該 cascade の VP で更新する。
    void SetCurrentCascade(u32 cascade) noexcept;

    // キャスター描画ごとのモデル行列を設定
    void SetCaster(const FMat4& model) noexcept;

    IRhiTexture*  DepthTexture()   const noexcept { return _depth.Get(); }
    IRhiPipeline* CasterPipeline() const noexcept { return _pipeline.Get(); }
    IRhiBuffer*   LightCB()        const noexcept { return _light_cb.Get(); }   // b0 = light_vp
    IRhiBuffer*   CasterObjectCB() const noexcept { return _object_cb.Get(); } // b1 = model

    // single cascade 用の後方互換 getter
    FMat4 LightViewProjection() const noexcept { return _light_vp[0]; }

    // CSM 用 (cascade 0..CascadeCount()-1)
    FMat4 LightViewProjection(u32 cascade) const noexcept {
        return _light_vp[cascade < _cascade_count ? cascade : 0];
    }
    f32 CascadeSplit(u32 cascade) const noexcept {
        return _cascade_splits[cascade < _cascade_count ? cascade : 0];
    }
    u32 CascadeCount() const noexcept { return _cascade_count; }

    // atlas 内の cascade 領域に対する viewport / scissor。BeginShadowPass の
    // 後・各 caster 群描画の前に SetViewport / SetScissor へ渡す。
    FViewport    CascadeViewport(u32 cascade) const noexcept;
    FScissorRect CascadeScissor (u32 cascade) const noexcept;

    u32 Size() const noexcept { return _size; }

private:
    TUniquePtr<IRhiTexture>  _depth;
    TUniquePtr<IRhiShader>   _vs;
    TUniquePtr<IRhiPipeline> _pipeline;
    TUniquePtr<IRhiBuffer>   _light_cb;
    TUniquePtr<IRhiBuffer>   _object_cb;
    FMat4                    _light_vp      [kMaxCascades] = {};
    f32                     _cascade_splits[kMaxCascades] = {};
    u32                     _size          = 0;
    u32                     _cascade_count = 1;
};

} // namespace acs
