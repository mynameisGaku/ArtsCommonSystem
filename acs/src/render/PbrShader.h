// SPDX-License-Identifier: Apache-2.0
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

    // Normal map (Phase 34g) を設定。null で fallback (flat normal、無変化)。
    // ddx/ddy 由来の screen-space TBN を shader が自前で計算するので、
    // 頂点 tangent は不要。tileable な RGB8 (DirectX 流: (R,G,B)=tangent×0.5+0.5) 想定。
    void SetNormalMap(IRhiTexture* tex) noexcept;

    // SSAO map (Phase 34j-2)。screen-space AO の visibility テクスチャ。
    //   ssao_tex: Ssao::OutputTexture() (R8G8B8A8_UNorm、.r=visibility)。null で OFF。
    //   intensity: 0=neutral (AO 無視)、1=通常、>1=AO 強調
    //   viewport_w/h: tonemap 前の HDR RT サイズ (pixel)。0/0 で SSAO を強制 OFF。
    void SetSsao(IRhiTexture* ssao_tex, f32 intensity,
                 u32 viewport_w, u32 viewport_h) noexcept;

    // SSGI color (Phase 33c)。1 bounce indirect light の screen-space 推定。
    //   ssgi_tex: Ssgi::OutputTexture() (R11G11B10F)。null で OFF。
    //   intensity: 0=indirect 無視、1=通常、>1=強調 (経験的に 0.5..2.0)
    // viewport_inv_size は SSAO の値を再利用するので別途渡さない。
    void SetSsgi(IRhiTexture* ssgi_tex, f32 intensity) noexcept;

    // SSR (Phase 34e-2fix): screen-space reflection を IBL specular に blend する。
    //   ssr_tex: Ssr::OutputTexture() (HDR、.rgb=反射放射輝度、.a=hit mask)。null で OFF。
    //   intensity: 0=OFF、1=通常。roughness が高い面ほど自動で寄与が下がる
    //              (rough 面は環境 prefilter、smooth 面は SSR を反射元に使う)。
    // 物理的に正しい roughness 依存反射のため、tonemap 合成ではなく PbrShader 側で
    // 合成する (SSAO/SSGI と同じ方式)。viewport は SSAO の値を再利用。
    void SetSsr(IRhiTexture* ssr_tex, f32 intensity) noexcept;

    // Lightmap (Phase 33f)。baked static GI texture を mesh の uv で sample。
    //   lightmap_tex: 静的に焼かれた間接光 (任意フォーマット、RGB が使われる)
    //   intensity: 0=OFF、1=通常、>1=強調
    // 注意: mesh の uv が global lightmap 座標系として作られている前提。本実装
    // では uv2 を別途持たず単一 uv で済ませる (Cornell box 等の per-instance 用途)。
    void SetLightmap(IRhiTexture* lightmap_tex, f32 intensity) noexcept;

    // SH 9 (Ramamoorthi) ambient mode を有効化する (Phase 32c)。
    // `sh9` は ImageBasedLighting::ComputeSh9FromEquirect で得られた 9 RGB 係数。
    // 有効化中は cubemap irradiance ではなく SH 9 から復元した diffuse irradiance を使う。
    // SetSh9(nullptr) で OFF に戻す。SetIbl が前段で呼ばれてないと意味がない。
    void SetSh9(const Vec4* sh9_or_null) noexcept;

    // 静的光プローブグリッド (Phase 33d)。各 probe = (位置 + SH9 9 係数)。
    // count > 0 のとき、PbrShader は SH9 single mode より優先して probe grid を使う
    // (world position から IDW で blend)。
    //   probe_positions: 4 個まで、xyz=world、w=未使用
    //   probe_sh9      : probe ごとに連続した 9 個 (count * 9 個の Vec4)
    struct LightProbe {
        Vec3 position;
        Vec4 sh9[9];     // xyz=RGB
    };
    void SetProbeGrid(const LightProbe* probes, u32 count) noexcept;

    // Volumetric exponential height fog (Phase 33e)。
    //   color    : fog 色 (HDR scale も可)
    //   density  : 単位距離あたりの吸収率。0 で fog OFF
    //   height_falloff: y 軸方向の指数減衰。> 0 で上空ほど薄く、地面近くで濃く
    //   height_base   : fog 基準 y (この高さで density フル、上で減衰開始)
    void SetFog(Vec3 color, f32 density,
                f32 height_falloff = 0.5f, f32 height_base = 0.0f) noexcept;

    // Shadow map (Phase 34b、第 0 番目 dir light のみ)。
    // depth: ShadowMap::DepthTexture()、light_vp: ShadowMap::LightViewProjection()
    // null で OFF。bias は 0.001..0.01 程度 (acne 回避)。
    //   filter_radius (Phase 34b-2): PCSS 強度。0 = hard PCF、1 = 標準、>1 で柔らか
    //
    // **重要**: shadow を disable したいときは必ず `SetShadowMap(nullptr, ...)` を使うこと。
    // `_shadow_params.y` を直接 0 にして再有効化する経路は texel_size=0 の場合 PCSS の
    // blocker search で search_r=0 になり結果が壊れる。常にこの API 経由で更新する。
    void SetShadowMap(IRhiTexture* depth, const Mat4& light_vp,
                      f32 bias = 0.002f, f32 texel_size = 1.0f / 2048.0f,
                      f32 filter_radius = 1.0f) noexcept;

    // CSM 用 (Phase 34b part 3): 複数 cascade の VP と split を一括設定。
    //   light_vp[c]      : 各 cascade の light VP (c=0..cascade_count-1)
    //   cascade_splits[c]: 各 cascade の view-space z far (cascade 選択の閾値)
    //   cascade_count    : 使用する cascade 数 (1..4)
    //   bias / texel_size: PCSS パラメータ (single API と同じ意味)
    // ShadowMap が atlas で確保した深度テクスチャを渡し、HLSL 側で view_z から
    // cascade を選択 + atlas UV 変換でサンプルする。null depth で disable。
    void SetShadowMapCascades(IRhiTexture* depth,
                               const Mat4* light_vp,
                               const f32*  cascade_splits,
                               u32 cascade_count,
                               f32 bias = 0.002f,
                               f32 texel_size = 1.0f / 2048.0f,
                               f32 filter_radius = 1.0f) noexcept;

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

    // Emissive (Phase 34l): 自己発光色。lighting とは無関係に最終色へ加算され、
    // HDR パイプラインで tonemap / bloom に乗る (strength > 1 で発光が bloom する)。
    // SetExtParams と同じく member に格納され、次の SetObject / DrawMesh が反映する。
    // SetEmissive(Vec3{0,0,0}, 0) で無効化。既定は無効 (他 sample に影響しない)。
    void SetEmissive(Vec3 color, f32 strength = 1.0f) noexcept;

    // Sheen (Phase 35-1a): 布/ベルベットの retro-reflective fuzz 層。base BRDF に
    // 加算され、エネルギー保存のため base を簡易減衰させる。
    //   sheen_color: 毛羽の色 (RGB)。weight=0 で無効 (既定、他 sample に影響しない)。
    //   weight     : 0=OFF、1=フル。roughness: Charlie 分布の幅 (大きいほど soft)。
    // SetExtParams と同じく member に格納され、次の SetObject / DrawMesh が反映する。
    void SetSheen(Vec3 sheen_color, f32 weight, f32 roughness = 0.3f) noexcept;

    // Iridescence (Phase 35-1b): 薄膜干渉。シャボン玉/タマムシ的に視角で色が変わる。
    // base の Fresnel を Belcour-Barla 2017 の薄膜モデルで変調する。
    //   weight      : 0=OFF (既定、他 sample に影響しない)、1=フル。
    //   thickness_nm: 膜厚 (nm、~100-1000)。film_ior: 薄膜の屈折率 (シャボン~1.33)。
    void SetIridescence(f32 weight, f32 thickness_nm = 400.0f,
                        f32 film_ior = 1.4f) noexcept;

    // Subsurface scattering (Phase 35-2): 肌/ロウ/大理石のような内部散乱の質感。
    // wrapped diffuse (terminator の柔らかさ) + 裏面 translucency (逆光の透け) を
    // 解析近似で直接光に加算する。LUT・追加 pass 不要。
    //   sss_color: 内部散乱の色 (肌なら赤み)。weight=0 で無効 (既定、他 sample 無影響)。
    void SetSubsurface(Vec3 sss_color, f32 weight) noexcept;

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
    UniquePtr<IRhiTexture>  _normal_map_fb;          // 1x1   RGBA8 flat normal

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

    // 静的光プローブグリッド (Phase 33d)、最大 4 個
    Vec4         _probe_pos[4]      = {};
    Vec4         _probe_sh9[4 * 9]  = {};
    u32          _probe_count       = 0;

    // Volumetric fog (Phase 33e)
    Vec4         _fog_color_density = Vec4{0, 0, 0, 0};
    Vec4         _fog_height_params = Vec4{0.5f, 0, 0, 0};

    // Normal map (Phase 34g)、SetNormalMap で差し替え可能
    IRhiTexture* _normal_map = nullptr;

    // SSAO map (Phase 34j-2)、SetSsao で差し替え可能
    IRhiTexture* _ssao_tex     = nullptr;
    f32          _ssao_intensity = 0.0f;
    f32          _ssao_inv_w   = 0.0f;
    f32          _ssao_inv_h   = 0.0f;
    UniquePtr<IRhiTexture> _ssao_fb;     // 1x1 全 255 fallback (visibility=1)

    // SSGI color (Phase 33c)、SetSsgi で差し替え可能
    IRhiTexture* _ssgi_tex       = nullptr;
    f32          _ssgi_intensity = 0.0f;
    UniquePtr<IRhiTexture> _ssgi_fb;    // 1x1 R11G11B10F fallback (initial 0)

    // SSR (Phase 34e-2fix)、SetSsr で差し替え可能
    IRhiTexture* _ssr_tex       = nullptr;
    f32          _ssr_intensity = 0.0f;
    UniquePtr<IRhiTexture> _ssr_fb;     // 1x1 RGBA8 黒 (hit mask 0) fallback

    // Lightmap (Phase 33f)、SetLightmap で差し替え可能
    IRhiTexture* _lightmap_tex       = nullptr;
    f32          _lightmap_intensity = 0.0f;
    UniquePtr<IRhiTexture> _lightmap_fb;  // 1x1 RGBA8 黒 fallback

    // Shadow map (Phase 34b + CSM = Phase 34b part 3)。
    // ShadowMap::kMaxCascades と一致 (現状 4)。Frame CB の shadow_view_proj[4] と対応。
    static constexpr u32 kMaxShadowCascades = 4;
    IRhiTexture* _shadow_depth          = nullptr;
    Mat4         _shadow_view_proj[kMaxShadowCascades] = {};   // 各 cascade の light VP
    Vec4         _shadow_params         = Vec4{0.002f, 0, 1.0f/2048.0f, 0};
    Vec4         _cascade_splits        = Vec4{1e30f, 1e30f, 1e30f, 1e30f};  // single mode: 常に cascade 0
    Vec4         _cascade_uv_scale      = Vec4{1.0f, 1.0f, 0, 0};            // single mode: atlas 変換無し
    UniquePtr<IRhiTexture> _shadow_fb;   // fallback (1x1 depth-ish texture、未バインド時用)

    // 拡張 material (Phase 33a)、SetObject で reset、SetExtParams で上書き
    Vec4         _ext_params     = Vec4{0, 0.5f, 0, 0};
    Vec4         _aniso_tangent  = Vec4{1, 0, 0, 0};

    // Emissive (Phase 34l)、SetEmissive で上書き。xyz=color*strength、既定 0 (無効)
    Vec4         _emissive       = Vec4{0, 0, 0, 0};

    // Sheen (Phase 35-1a)、SetSheen で上書き。xyz=color, w=weight (既定 0=OFF)
    Vec4         _sheen_params   = Vec4{0, 0, 0, 0};
    Vec4         _sheen_rough    = Vec4{0.3f, 0, 0, 0};

    // Iridescence (Phase 35-1b)、SetIridescence で上書き。
    // x=weight (既定 0=OFF)、y=thickness(nm)、z=film IOR
    Vec4         _irid_params    = Vec4{0, 400.0f, 1.4f, 0};

    // Subsurface (Phase 35-2)、SetSubsurface で上書き。xyz=color, w=weight (既定 0=OFF)
    Vec4         _sss_params     = Vec4{0, 0, 0, 0};
};

} // namespace acs
