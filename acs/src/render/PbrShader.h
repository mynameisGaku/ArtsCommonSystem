// PBR (Cook-Torrance BRDF) ライティングシェーダ — Metalness/Roughness workflow
//
// 用途: メッシュアセット (位置 + 法線 + UV) を、複数の有向光源 + 環境光 +
//       PBR 反射モデル + アルベドテクスチャで描画する。
//
// StandardShader (Blinn-Phong) と並走する形で導入。既存 StandardShader を
// 使ってる sample はそのまま、新規 sample (HelloPbr 等) で PbrShader を選ぶ。
//
// 使い方:
//   PbrShader shd;
//   shd.Init(*renderer.Device(), renderer.ColorFormat(), renderer.DepthFormat());
//   shd.SetLights(camera.ViewProjection(), camera.Eye(),
//                 lights, 1, ambient_color);
//   shd.SetObject(model_mat, base_color, /*metallic=*/0.0f, /*roughness=*/0.5f,
//                 /*ao=*/1.0f);
//   cl->SetPipeline(*shd.Pipeline());
//   cl->SetConstantBuffer(0, *shd.PerFrameCB());
//   cl->SetConstantBuffer(1, *shd.PerObjectCB());
//   cl->SetTexture(0, *shd.DefaultWhiteTexture());
//   cl->SetVertexBuffer(*gm.vertex_buffer, gm.vertex_stride);
//   cl->SetIndexBuffer(*gm.index_buffer);
//   cl->DrawIndexed(gm.index_count);
//
// BRDF:
//   ・GGX (Trowbridge-Reitz) normal distribution
//   ・Smith joint geometry (Schlick-GGX approximation)
//   ・Schlick Fresnel
//   ・energy-conserving Lambertian diffuse
//
// material parameters:
//   ・base_color: 非金属の albedo + 金属の reflectance tint
//   ・metallic:   0 = dielectric (誘電体)、1 = metal
//   ・roughness:  0 = mirror-smooth、1 = completely rough
//   ・ao:         ambient occlusion (1 = no occlusion)
//
// 将来拡張: IBL (Phase 31) / GI (Phase 32) を Frame CB に追加。
#pragma once

#include "render/RenderAssets.h"        // GpuMesh
#include "render/IRhiCommandList.h"
#include "render/StandardShader.h"      // DirLight / PointLight を再利用

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiTexture.h"
#include "render/RhiTypes.h"

namespace acs {

class PbrShader {
public:
    PbrShader() noexcept = default;
    ~PbrShader() noexcept = default;

    PbrShader(const PbrShader&)            = delete;
    PbrShader& operator=(const PbrShader&) = delete;

    Result<void> Init(IRhiDevice& device,
                      Format rt_format    = Format::B8G8R8A8_UNorm,
                      Format depth_format = Format::D32_Float) noexcept;
    void Shutdown() noexcept;

    void SetLights(const Mat4& view_projection,
                   Vec3 camera_pos,
                   const DirLight* lights, u32 count,
                   Vec3 ambient_color) noexcept;
    void SetPointLights(const PointLight* lights, u32 count) noexcept;

    // PBR material 設定。base_color は非金属時の albedo、金属時の F0 tint。
    // metallic [0,1], roughness [0,1] は線形 (perceptual ではなく直接 GGX に
    // 渡す)、ao [0,1] は ambient のみに乗算 (direct light には影響しない)。
    void SetObject(const Mat4& model,
                   Vec3 base_color = Vec3{1, 1, 1},
                   f32  metallic   = 0.0f,
                   f32  roughness  = 0.5f,
                   f32  ao         = 1.0f) noexcept;

    IRhiPipeline* Pipeline()    const noexcept { return _pipeline.Get(); }
    IRhiBuffer*   PerFrameCB()  const noexcept { return _frame_cb.Get(); }
    IRhiBuffer*   PerObjectCB() const noexcept { return _object_cb.Get(); }
    IRhiTexture*  DefaultWhiteTexture() const noexcept { return _white.Get(); }

    // SetPipeline + CB + Tex + VB/IB + DrawIndexed をまとめた便利 API。
    void DrawMesh(IRhiCommandList& cmd,
                  const GpuMesh& mesh,
                  const Mat4& model,
                  Vec3 base_color = Vec3{1, 1, 1},
                  f32  metallic   = 0.0f,
                  f32  roughness  = 0.5f,
                  f32  ao         = 1.0f,
                  IRhiTexture* albedo = nullptr) noexcept;

private:
    void FlushFrameCB() noexcept;

    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiPipeline> _pipeline;
    UniquePtr<IRhiBuffer>   _frame_cb;
    UniquePtr<IRhiBuffer>   _object_cb;
    UniquePtr<IRhiTexture>  _white;

    Mat4       _vp;
    Vec3       _eye      = Vec3{0, 0, 0};
    Vec3       _ambient  = Vec3{0, 0, 0};
    DirLight   _dir_lights[4];
    u32        _dir_count = 0;
    PointLight _point_lights[4];
    u32        _point_count = 0;
};

} // namespace acs
