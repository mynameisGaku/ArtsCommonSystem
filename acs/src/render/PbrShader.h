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

    // 矩形 area light (Phase 33b)。
    //   center    : 矩形中心の world position
    //   axis_x    : 矩形の +X 方向 半幅ベクトル (= 右方向 × half_width)
    //   axis_y    : 矩形の +Y 方向 半高ベクトル (= 上方向 × half_height)
    //   color     : 放射輝度 (W/m²/sr スケールで HDR 値 OK)
    // 法線方向は cross(axis_x, axis_y) 正規化、その逆方向に光を放つ (片面 emit)。
    struct AreaLight {
        Vec3 center;
        Vec3 axis_x;
        Vec3 axis_y;
        Vec3 color;
    };
    void SetAreaLights(const AreaLight* lights, u32 count) noexcept;

    // IBL textures をバインドする (任意)。
    // irradiance/prefilter/brdf_lut 3 つともに非 null かつ prefilter_mips > 0
    // のときに IBL ambient が有効化される。それ以外は flat ambient (= 既存挙動)。
    // 通常は `ImageBasedLighting::IrradianceMap()` / `PrefilterMap()` /
    // `BrdfLut()` / `PrefilterMips()` をそのまま渡せばよい。
    void SetIbl(IRhiTexture* irradiance,
                IRhiTexture* prefilter,
                IRhiTexture* brdf_lut,
                u32 prefilter_mips) noexcept;

    // SetTexture(0, albedo) と並んで IBL slot 1/2/3 を bind するヘルパ。
    // SetIbl で非 null をセットしてあれば実テクスチャ、そうでなければ fallback
    // (1x1 black cubemap / 2D) を bind する。
    void BindIblTextures(IRhiCommandList& cmd) noexcept;

    // SH 9 (Ramamoorthi) ambient mode を有効化する (Phase 32c)。
    // `sh9` は ImageBasedLighting::ComputeSh9FromEquirect で得られた 9 RGB 係数。
    // 有効化中は cubemap irradiance ではなく SH 9 から復元した diffuse irradiance を使う。
    // SetSh9(nullptr) で OFF に戻す。SetIbl が前段で呼ばれてないと意味がない。
    void SetSh9(const Vec4* sh9_or_null) noexcept;

    // PBR material 設定。base_color は非金属時の albedo、金属時の F0 tint。
    // metallic [0,1], roughness [0,1] は線形 (perceptual ではなく直接 GGX に
    // 渡す)、ao [0,1] は ambient のみに乗算 (direct light には影響しない)。
    void SetObject(const Mat4& model,
                   Vec3 base_color = Vec3{1, 1, 1},
                   f32  metallic   = 0.0f,
                   f32  roughness  = 0.5f,
                   f32  ao         = 1.0f) noexcept;

    // 拡張 material (Phase 33a)。次回 SetObject() で reset されるので、
    // SetObject の直後に呼ぶこと。または SetObjectEx() を使う。
    //   clearcoat (0..1)     : 透明な top-coat ラッカー強度
    //   clearcoat_roughness  : top-coat の粗さ (0=mirror, 1=matte)
    //   anisotropy (-1..1)   : 0=isotropic、±で tangent/bitangent 方向に伸びる
    //   tangent              : anisotropic 主軸 world direction (anisotropy=0 なら未使用)
    void SetExtParams(f32 clearcoat, f32 clearcoat_roughness,
                      f32 anisotropy, Vec3 tangent = Vec3{1, 0, 0}) noexcept;

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
    // IBL を使わないときに texture slot 1-3 に bind するための fallback。
    // shader は ibl_params.x で uniform branching し、ここを sample しないが
    // SRB の bind は必須。1x1 cubemap / 1x1 2D で、内容は undefined (zero 化される)。
    UniquePtr<IRhiTexture>  _ibl_irradiance_fb;     // 1x1x6 R11G11B10F cubemap
    UniquePtr<IRhiTexture>  _ibl_prefilter_fb;       // 1x1x6 R11G11B10F cubemap
    UniquePtr<IRhiTexture>  _ibl_brdf_fb;            // 1x1   RG16F 2D

    Mat4       _vp;
    Vec3       _eye      = Vec3{0, 0, 0};
    Vec3       _ambient  = Vec3{0, 0, 0};
    DirLight   _dir_lights[4];
    u32        _dir_count = 0;
    PointLight _point_lights[4];
    u32        _point_count = 0;
    AreaLight  _area_lights[2];
    u32        _area_count = 0;

    // SetIbl で渡された (非所有) ポインタ。3 つとも非 null かつ _ibl_mips > 0 なら有効。
    IRhiTexture* _ibl_irradiance = nullptr;
    IRhiTexture* _ibl_prefilter  = nullptr;
    IRhiTexture* _ibl_brdf       = nullptr;
    u32          _ibl_mips       = 0;
    bool         _ibl_enabled    = false;

    // SH 9 light probe (optional Phase 32c)
    Vec4         _sh9[9]         = {};
    bool         _sh9_enabled    = false;

    // 拡張 material (Phase 33a)、SetObject で reset、SetExtParams で上書き
    Vec4         _ext_params     = Vec4{0, 0.5f, 0, 0};
    Vec4         _aniso_tangent  = Vec4{1, 0, 0, 0};
};

} // namespace acs
