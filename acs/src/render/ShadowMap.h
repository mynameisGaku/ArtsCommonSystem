// シャドウマップ（有向光源用、orthographic）
//
// 設計:
//   - 深度バッファをライトカメラ視点で描画 → 主シェーダで投影サンプリング
//   - 1 灯のみ（dir light の最初のもの）対応
//   - PCF 等の高品質フィルタは v2、現状は単純比較（バイアス付き）
//
// 使い方:
//   ShadowMap sm;
//   sm.Init(*dev, /*size=*/2048);
//
//   // フレームごと:
//   //   1. ライトの方向 + シーン中心 + 半径から視推影行列を計算
//   sm.SetDirectionalLight(light_dir, Vec3{0, 0, 0}, /*radius=*/15.0f);
//
//   //   2. シャドウパス開始 → メッシュ描画 → 終了
//   cl->BeginShadowPass(*sm.DepthTexture(), 1.0f);
//   cl->SetPipeline(*sm.CasterPipeline());
//   cl->SetConstantBuffer(0, *sm.LightCB());
//   for (each shadow caster) {
//       sm.SetCaster(model_mat);
//       cl->SetConstantBuffer(1, *sm.CasterObjectCB());
//       cl->SetVertexBuffer(*gm.vertex_buffer, gm.vertex_stride);
//       cl->SetIndexBuffer (*gm.index_buffer);
//       cl->DrawIndexed(gm.index_count);
//   }
//   cl->EndShadowPass(*sm.DepthTexture());
//
//   //   3. 主パスで使用:
//   //     - StandardShader::SetShadowMap(*sm.DepthTexture(), sm.LightViewProjection());
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

namespace acs {

class ShadowMap {
public:
    ShadowMap() noexcept = default;
    ~ShadowMap() noexcept = default;

    ShadowMap(const ShadowMap&)            = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    Result<void> Init(IRhiDevice& device, u32 size = 2048) noexcept;
    void Shutdown() noexcept;

    // 有向光源のためにライト視点の orthographic 投影行列を計算
    // light_dir: シーンから光源へ向かう方向（StandardShader と同じ慣習）
    void SetDirectionalLight(Vec3 light_dir,
                              Vec3 scene_center,
                              f32 scene_radius) noexcept;

    // キャスター描画ごとのモデル行列を設定（CB に書き込み）
    void SetCaster(const Mat4& model) noexcept;

    IRhiTexture*  DepthTexture()   const noexcept { return _depth.Get(); }
    IRhiPipeline* CasterPipeline() const noexcept { return _pipeline.Get(); }
    IRhiBuffer*   LightCB()        const noexcept { return _light_cb.Get(); }   // b0 = light_vp
    IRhiBuffer*   CasterObjectCB() const noexcept { return _object_cb.Get(); } // b1 = model

    Mat4 LightViewProjection() const noexcept { return _light_vp; }
    u32  Size() const noexcept { return _size; }

private:
    UniquePtr<IRhiTexture>  _depth;
    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiPipeline> _pipeline;
    UniquePtr<IRhiBuffer>   _light_cb;
    UniquePtr<IRhiBuffer>   _object_cb;
    Mat4                    _light_vp;
    u32                     _size = 0;
};

} // namespace acs
