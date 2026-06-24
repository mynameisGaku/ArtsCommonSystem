// SPDX-License-Identifier: Apache-2.0
// PBR (Cook-Torrance BRDF) ライティングシェーダ — Metalness/Roughness workflow
//
// 用途: メッシュアセット (位置 + 法線 + UV) を、複数の有向光源 + 環境光 +
//       PBR 反射モデル + アルベドテクスチャで描画する。
//
// FStandardShader (Blinn-Phong) と並走する形で導入。既存 FStandardShader を
// 使ってる sample はそのまま、新規 sample (HelloPbr 等) で FPbrShader を選ぶ。
//
// 使い方:
//   FPbrShader shd;
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

/**
 * Cook-Torrance BRDF (GGX + Smith + Schlick) PBR ライティングシェーダ。
 *
 * @details
 * Metalness/Roughness workflow でメッシュ (位置 + 法線 + UV) を複数の有向光源/
 * 点光源/矩形 area light + 環境光で描画する。IBL (irradiance/prefilter/BRDF LUT)、
 * SH9/probe grid ambient、normal map、SSAO/SSGI/SSR/lightmap、CSM shadow、
 * volumetric fog、emissive、clearcoat/anisotropy/sheen/iridescence/subsurface の
 * 拡張マテリアルを束ね、per-frame / per-object の 2 定数バッファで駆動する。
 */
class FPbrShader {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FPbrShader() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FPbrShader() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FPbrShader(const FPbrShader&)            = delete;

    /** コピー代入も禁止。 */
    FPbrShader& operator=(const FPbrShader&) = delete;

    /**
     * シェーダ・パイプライン・定数バッファ・fallback テクスチャ群を生成する。
     *
     * @param device 生成に使う RHI デバイス。
     * @param rt_format レンダーターゲットのフォーマット (既定 B8G8R8A8_UNorm)。
     * @param depth_format 深度バッファのフォーマット (既定 D32_Float)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float,
                      ECullMode cull_mode  = ECullMode::Back) noexcept;

    /** 確保した GPU リソースを解放する (多重呼び出し安全)。 */
    void Shutdown() noexcept;

    /**
     * 有向光源と camera・環境光を設定する (frame CB を更新)。
     *
     * @param view_projection カメラの view-projection 行列。
     * @param camera_pos カメラの world position (specular の視線方向に使う)。
     * @param lights 有向光源の配列。
     * @param count 有向光源数 (最大 4、超過分は切り捨て)。
     * @param ambient_color flat ambient の色 (IBL 無効時に使う)。
     */
    void SetLights(const FMat4& view_projection,
                   FVec3 camera_pos,
                   const FDirLight* lights, u32 count,
                   FVec3 ambient_color) noexcept;

    /**
     * 点光源を設定する (frame CB を更新)。
     *
     * @param lights 点光源の配列。
     * @param count 点光源数 (最大 4、超過分は切り捨て)。
     */
    void SetPointLights(const PointLight* lights, u32 count) noexcept;

    /**
     * 矩形 area light の記述。
     *
     * @details
     * center は矩形中心の world position、axis_x/axis_y は +X/+Y 方向の半幅/半高
     * ベクトル (= 方向 × half_size)、color は放射輝度 (HDR 可)。法線は
     * cross(axis_x, axis_y) 正規化方向で、その逆方向に光を放つ片面 emit。
     */
    struct AreaLight {
        /** 矩形中心の world position。 */
        FVec3 center;

        /** 矩形の +X 方向 半幅ベクトル (= 右方向 × half_width)。 */
        FVec3 axis_x;

        /** 矩形の +Y 方向 半高ベクトル (= 上方向 × half_height)。 */
        FVec3 axis_y;

        /** 放射輝度 (W/m²/sr スケールで HDR 値 OK)。 */
        FVec3 color;
    };

    /**
     * 矩形 area light を設定する (frame CB を更新)。
     *
     * @param lights area light の配列。
     * @param count area light 数 (最大 2、超過分は切り捨て)。
     */
    void SetAreaLights(const AreaLight* lights, u32 count) noexcept;

    /**
     * IBL テクスチャをバインドする (任意)。
     *
     * @details
     * 3 つとも非 null かつ prefilter_mips > 0 のときに IBL ambient が有効化される。
     * それ以外は flat ambient (= 既存挙動)。通常は ImageBasedLighting の
     * IrradianceMap()/PrefilterMap()/BrdfLut()/PrefilterMips() をそのまま渡す。
     * @param irradiance 拡散 irradiance cubemap。
     * @param prefilter pre-filtered 環境 specular cubemap。
     * @param brdf_lut split-sum BRDF LUT (RG16F)。
     * @param prefilter_mips prefilter cubemap の mip 数。
     */
    void SetIbl(IRhiTexture* irradiance,
                IRhiTexture* prefilter,
                IRhiTexture* brdf_lut,
                u32 prefilter_mips) noexcept;

    /**
     * IBL slot 1/2/3 と normal/shadow/SSAO/SSGI/lightmap/SSR の各 slot を bind する。
     *
     * @details SetIbl で非 null をセットしてあれば実テクスチャ、無ければ fallback
     * (1x1 black cubemap / 2D) を bind する。SetTexture(0, albedo) と並べて使う。
     * @param cmd テクスチャを bind するコマンドリスト。
     */
    void BindIblTextures(IRhiCommandList& cmd) noexcept;

    /**
     * Normal map を設定する。
     *
     * @details null で fallback (flat normal、無変化)。ddx/ddy 由来の screen-space TBN を
     * shader が自前で計算するので頂点 tangent は不要。tileable な RGB8
     * (DirectX 流: (R,G,B)=tangent×0.5+0.5) を想定。
     * @param tex normal map テクスチャ (null で無効)。
     */
    void SetNormalMap(IRhiTexture* tex) noexcept;

    /**
     * SSAO の visibility テクスチャを設定する (ambient/indirect 項に乗算)。
     *
     * @param ssao_tex FSsao::OutputTexture() (R8G8B8A8_UNorm、.r=visibility)。null で OFF。
     * @param intensity 0=neutral (AO 無視)、1=通常、>1=AO 強調。
     * @param viewport_w tonemap 前の HDR RT 幅 (pixel)。0 で SSAO 強制 OFF。
     * @param viewport_h tonemap 前の HDR RT 高さ (pixel)。0 で SSAO 強制 OFF。
     */
    void SetSsao(IRhiTexture* ssao_tex, f32 intensity,
                 u32 viewport_w, u32 viewport_h) noexcept;

    /**
     * SSGI color (1 bounce indirect light の screen-space 推定) を設定する。
     *
     * @details viewport inv size は SSAO の値を再利用するので別途渡さない。
     * @param ssgi_tex FSsgi::OutputTexture() (R11G11B10F)。null で OFF。
     * @param intensity 0=indirect 無視、1=通常、>1=強調 (経験的に 0.5..2.0)。
     */
    void SetSsgi(IRhiTexture* ssgi_tex, f32 intensity) noexcept;

    /**
     * SSR (screen-space reflection) を IBL specular に blend する設定をする。
     *
     * @details
     * roughness が高い面ほど自動で寄与が下がる (rough 面は環境 prefilter、smooth 面は
     * SSR を反射元に使う)。物理的に正しい roughness 依存反射のため tonemap 合成ではなく
     * FPbrShader 側で合成する。viewport は SSAO の値を再利用する。
     * @param ssr_tex FSsr::OutputTexture() (HDR、.rgb=反射放射輝度、.a=hit mask)。null で OFF。
     * @param intensity 0=OFF、1=通常。
     */
    void SetSsr(IRhiTexture* ssr_tex, f32 intensity) noexcept;

    /**
     * Aerial perspective camera-volume LUT を設定する (WickedEngine 流の大気の距離霞)。
     *
     * @param ap_vol AP froxel volume (FSkyAtmosphere::BuildAerialPerspective の出力)。nullptr で無効。
     * @param max_dist volume がカバーする最大距離 (scene 単位、深度→スライス逆変換に使う)。
     */
    void SetAerialPerspective(IRhiTexture* ap_vol, f32 max_dist) noexcept;

    /**
     * Lightmap (baked static GI) を mesh の uv で sample する設定をする。
     *
     * @details mesh の uv が global lightmap 座標系として作られている前提。本実装は
     * uv2 を別途持たず単一 uv で済ませる (Cornell box 等の per-instance 用途)。
     * @param lightmap_tex 静的に焼かれた間接光 (任意フォーマット、RGB が使われる)。null で OFF。
     * @param intensity 0=OFF、1=通常、>1=強調。
     */
    void SetLightmap(IRhiTexture* lightmap_tex, f32 intensity) noexcept;

    /**
     * SH 9 (Ramamoorthi) ambient mode を有効化する。
     *
     * @details
     * 有効化中は cubemap irradiance ではなく SH 9 から復元した diffuse irradiance を使う。
     * SetSh9(nullptr) で OFF に戻す。SetIbl が前段で呼ばれてないと意味がない。
     * @param sh9_or_null ComputeSh9FromEquirect で得た 9 RGB 係数。null で OFF。
     */
    void SetSh9(const FVec4* sh9_or_null) noexcept;

    /**
     * 静的光プローブ (位置 + SH9 9 係数) の記述。
     *
     * @details FPbrShader は count > 0 のとき SH9 single mode より優先して probe grid を
     * 使い、world position から IDW で blend する。
     */
    struct LightProbe {
        /** probe の world position。 */
        FVec3 position;

        /** SH 9 係数 (各 xyz=RGB)。 */
        FVec4 sh9[9];
    };

    /**
     * 静的光プローブグリッドを設定する (frame CB を更新)。
     *
     * @param probes probe の配列 (各 probe = 位置 + SH9 9 係数)。
     * @param count probe 数 (最大 4、超過分は切り捨て)。
     */
    void SetProbeGrid(const LightProbe* probes, u32 count) noexcept;

    /**
     * 指数 height fog を設定する (frame CB を更新)。
     *
     * @param color fog 色 (HDR scale も可)。
     * @param density 単位距離あたりの吸収率 (0 で fog OFF)。
     * @param height_falloff y 軸方向の指数減衰 (> 0 で上空ほど薄く、地面近くで濃く)。
     * @param height_base fog 基準 y (この高さで density フル、上で減衰開始)。
     */
    void SetFog(FVec3 color, f32 density,
                f32 height_falloff = 0.5f, f32 height_base = 0.0f) noexcept;

    /**
     * Shadow map (第 0 番目 dir light のみ) を設定する。
     *
     * @details
     * shadow を disable したいときは必ず本 API に null depth を渡すこと。m_ShadowParams.y を
     * 直接 0 にして再有効化する経路は texel_size=0 の場合 PCSS blocker search で search_r=0 に
     * なり結果が壊れる。常にこの API 経由で更新する。
     * @param depth FShadowMap::DepthTexture()。null で OFF。
     * @param light_vp FShadowMap::LightViewProjection()。
     * @param bias depth bias (acne 回避、0.001..0.01 程度)。
     * @param texel_size shadow map 1 texel の UV サイズ。
     * @param filter_radius PCSS 強度 (0=hard PCF、1=標準、>1 で柔らか)。
     */
    void SetShadowMap(IRhiTexture* depth, const FMat4& light_vp,
                      f32 bias = 0.002f, f32 texel_size = 1.0f / 2048.0f,
                      f32 filter_radius = 1.0f) noexcept;

    /**
     * CSM 用に複数 cascade の VP と split を一括設定する。
     *
     * @details
     * FShadowMap が atlas で確保した深度テクスチャを渡し、HLSL 側で view_z から cascade を
     * 選択 + atlas UV 変換でサンプルする。
     * @param depth cascade atlas の深度テクスチャ。null で disable。
     * @param light_vp 各 cascade の light VP (c=0..cascade_count-1)。
     * @param cascade_splits 各 cascade の view-space z far (cascade 選択の閾値)。
     * @param cascade_count 使用する cascade 数 (1..4)。
     * @param bias depth bias (single API と同じ意味)。
     * @param texel_size cascade-local texel の UV サイズ。
     * @param filter_radius PCSS 強度 (single API と同じ意味)。
     */
    void SetShadowMapCascades(IRhiTexture* depth,
                               const FMat4* light_vp,
                               const f32*  cascade_splits,
                               u32 cascade_count,
                               f32 bias = 0.002f,
                               f32 texel_size = 1.0f / 2048.0f,
                               f32 filter_radius = 1.0f) noexcept;

    /**
     * PBR マテリアルを設定して per-object CB を更新する。
     *
     * @details metallic/roughness は線形 (perceptual ではなく直接 GGX に渡す)、ao は
     * ambient のみに乗算 (direct light には影響しない)。
     * @param model モデル行列。
     * @param base_color 非金属時の albedo、金属時の F0 tint。
     * @param metallic 金属度 [0,1] (0=誘電体、1=金属)。
     * @param roughness 粗さ [0,1] (0=mirror、1=completely rough)。
     * @param ao ambient occlusion [0,1] (1=遮蔽なし)。
     */
    void SetObject(const FMat4& model,
                   FVec3 base_color = FVec3{1, 1, 1},
                   f32  metallic   = 0.0f,
                   f32  roughness  = 0.5f,
                   f32  ao         = 1.0f) noexcept;

    /**
     * 拡張マテリアル (clearcoat / anisotropy) を member に格納する。
     *
     * @details
     * 値は member に格納され、次の SetObject / DrawMesh で CB に反映される。SetObject 単独では
     * 反映されないので、SetObject の前にこれらの拡張を設定しておく。
     * @param clearcoat 透明な top-coat ラッカー強度 (0..1)。
     * @param clearcoat_roughness top-coat の粗さ (0=mirror、1=matte)。
     * @param anisotropy 異方性 (-1..1。0=isotropic、±で tangent/bitangent 方向に伸びる)。
     * @param tangent anisotropic 主軸の world direction (anisotropy=0 なら未使用)。
     */
    void SetExtParams(f32 clearcoat, f32 clearcoat_roughness,
                      f32 anisotropy, FVec3 tangent = FVec3{1, 0, 0}) noexcept;

    /**
     * Emissive (自己発光色) を member に格納する。
     *
     * @details
     * lighting とは無関係に最終色へ加算され、HDR パイプラインで tonemap / bloom に乗る
     * (strength > 1 で発光が bloom する)。member に格納され次の SetObject / DrawMesh が反映する。
     * SetEmissive(FVec3{0,0,0}, 0) で無効化 (既定は無効)。
     * @param color 自己発光色 (RGB)。
     * @param strength 発光強度 (color に乗算)。
     */
    void SetEmissive(FVec3 color, f32 strength = 1.0f) noexcept;

    /**
     * Sheen (布/ベルベットの retro-reflective fuzz 層) を member に格納する。
     *
     * @details
     * base BRDF に加算され、エネルギー保存のため base を簡易減衰させる。member に格納され
     * 次の SetObject / DrawMesh が反映する (既定 weight=0 で無効)。
     * @param sheen_color 毛羽の色 (RGB)。
     * @param weight sheen 強度 (0=OFF、1=フル)。
     * @param roughness Charlie 分布の幅 (大きいほど soft)。
     */
    void SetSheen(FVec3 sheen_color, f32 weight, f32 roughness = 0.3f) noexcept;

    /**
     * Iridescence (薄膜干渉) を member に格納する。
     *
     * @details
     * base の Fresnel を Belcour-Barla 2017 の薄膜モデルで変調し、視角で色が変わる
     * シャボン玉/タマムシ的質感を出す。member に格納され次の SetObject / DrawMesh が反映する
     * (既定 weight=0 で無効)。
     * @param weight iridescence 強度 (0=OFF、1=フル)。
     * @param thickness_nm 膜厚 (nm、~100-1000)。
     * @param film_ior 薄膜の屈折率 (シャボン~1.33)。
     */
    void SetIridescence(f32 weight, f32 thickness_nm = 400.0f,
                        f32 film_ior = 1.4f) noexcept;

    /**
     * Subsurface scattering (内部散乱) を member に格納する。
     *
     * @details
     * 肌/ロウ/大理石のような質感を、wrapped diffuse (terminator の柔らかさ) + 裏面
     * translucency (逆光の透け) の解析近似で直接光に加算する。LUT・追加 pass 不要。
     * member に格納され次の SetObject / DrawMesh が反映する (既定 weight=0 で無効)。
     * @param sss_color 内部散乱の色 (肌なら赤み)。
     * @param weight SSS 強度 (0=OFF、1=フル)。
     */
    void SetSubsurface(FVec3 sss_color, f32 weight) noexcept;

    /**
     * 描画パイプラインを返す。
     *
     * @return PBR 描画の PSO。
     */
    IRhiPipeline* Pipeline()    const noexcept { return m_Pipeline.Get(); }

    /**
     * per-frame 定数バッファを返す。
     *
     * @return frame CB (b0、ライト・環境・shadow・post 設定)。
     */
    IRhiBuffer*   PerFrameCB()  const noexcept { return m_FrameCb.Get(); }

    /**
     * per-object 定数バッファを返す。
     *
     * @return object CB (b1、model・material 設定)。
     */
    IRhiBuffer*   PerObjectCB() const noexcept { return m_ObjectCbs[m_ObjRingIdx].Get(); }

    /**
     * albedo fallback の 1x1 白テクスチャを返す。
     *
     * @return 既定の白テクスチャ。
     */
    IRhiTexture*  DefaultWhiteTexture() const noexcept { return m_White.Get(); }

    /**
     * SetObject + パイプライン/CB/Tex/VB/IB bind + DrawIndexed をまとめて発行する。
     *
     * @param cmd コマンドを積むコマンドリスト。
     * @param mesh 描画する GPU mesh。
     * @param model モデル行列。
     * @param base_color 非金属時の albedo、金属時の F0 tint。
     * @param metallic 金属度 [0,1]。
     * @param roughness 粗さ [0,1]。
     * @param ao ambient occlusion [0,1]。
     * @param albedo albedo テクスチャ (null なら既定の白)。
     */
    void DrawMesh(IRhiCommandList& cmd,
                  const GpuMesh& mesh,
                  const FMat4& model,
                  FVec3 base_color = FVec3{1, 1, 1},
                  f32  metallic   = 0.0f,
                  f32  roughness  = 0.5f,
                  f32  ao         = 1.0f,
                  IRhiTexture* albedo = nullptr) noexcept;

private:
    /** 現在の member 値から frame CB レイアウトを構築して GPU に書き込む。 */
    void FlushFrameCB() noexcept;

    /** PBR 描画の頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** Cook-Torrance BRDF を評価するピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** PBR 描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** per-frame 定数バッファ (b0)。 */
    TUniquePtr<IRhiBuffer>   m_FrameCb;

    /** per-object 定数バッファ (b1) のリング。
     *  単一の frame-cycled CB だと «フレーム内の複数 SetObject» が同一スロットを上書きし、
     *  実行時に全 DrawIndexed が «最後の値» を読む (= 全メッシュが最後のノードの model/material に
     *  なる)。per-object に別バッファを巡回することで各 draw が自分の値を読む。SetLights で
     *  フレーム頭にリセット、SetObject ごとに次へ進める。 */
    static constexpr u32     kObjRing = 256;
    TUniquePtr<IRhiBuffer>   m_ObjectCbs[kObjRing];
    u32                      m_ObjRingIdx = 0;

    /** albedo fallback の 1x1 白テクスチャ。 */
    TUniquePtr<IRhiTexture>  m_White;

    /**
     * IBL 無効時に slot 1 へ bind する irradiance fallback (1x1x6 R11G11B10F cubemap)。
     *
     * @details shader は ibl_params.x で uniform branching し sample しないが、SRB の
     * bind は必須なので valid texture を確保する。内容は undefined (driver が 0 化)。
     */
    TUniquePtr<IRhiTexture>  m_IblIrradianceFb;

    /** IBL 無効時に slot 2 へ bind する prefilter fallback (1x1x6 R11G11B10F cubemap)。 */
    TUniquePtr<IRhiTexture>  m_IblPrefilterFb;

    /** IBL 無効時に slot 3 へ bind する BRDF LUT fallback (1x1 RG16F 2D)。 */
    TUniquePtr<IRhiTexture>  m_IblBrdfFb;

    /** normal map 無効時に slot 4 へ bind する fallback (1x1 RGBA8 flat normal)。 */
    TUniquePtr<IRhiTexture>  m_NormalMapFb;

    /** カメラの view-projection 行列。 */
    FMat4       m_Vp;

    /** カメラの world position (specular の視線方向)。 */
    FVec3       m_Eye      = FVec3{0, 0, 0};

    /** flat ambient の色 (IBL 無効時)。 */
    FVec3       m_Ambient  = FVec3{0, 0, 0};

    /** 有向光源 (最大 4)。 */
    FDirLight   m_DirLights[4];

    /** 有効な有向光源数。 */
    u32        m_DirCount = 0;

    /** 点光源 (最大 4)。 */
    PointLight m_PointLights[4];

    /** 有効な点光源数。 */
    u32        m_PointCount = 0;

    /** 矩形 area light (最大 2)。 */
    AreaLight  m_AreaLights[2];

    /** 有効な area light 数。 */
    u32        m_AreaCount = 0;

    /** SetIbl で渡された irradiance cubemap (非所有)。 */
    IRhiTexture* m_IblIrradiance = nullptr;

    /** SetIbl で渡された prefilter cubemap (非所有)。 */
    IRhiTexture* m_IblPrefilter  = nullptr;

    /** SetIbl で渡された BRDF LUT (非所有)。 */
    IRhiTexture* m_IblBrdf       = nullptr;

    /** prefilter cubemap の mip 数。 */
    u32          m_IblMips       = 0;

    /** IBL ambient 有効フラグ (3 テクスチャとも非 null かつ m_IblMips > 0 で true)。 */
    bool         m_IblEnabled    = false;

    /** SH 9 single mode の係数 (各 xyz=RGB、任意)。 */
    FVec4         m_Sh9[9]         = {};

    /** SH 9 single mode 有効フラグ。 */
    bool         m_bSh9Enabled    = false;

    /** 静的光プローブの world position (最大 4)。 */
    FVec4         m_ProbePos[4]      = {};

    /** 静的光プローブの SH 9 係数 (probe ごとに連続した 9 個)。 */
    FVec4         m_ProbeSh9[4 * 9]  = {};

    /** 有効な probe 数。 */
    u32          m_ProbeCount       = 0;

    /** fog の色と密度 (xyz=color、w=density)。 */
    FVec4         m_FogColorDensity = FVec4{0, 0, 0, 0};

    /** fog の高さパラメータ (x=height_falloff、y=height_base)。 */
    FVec4         m_FogHeightParams = FVec4{0.5f, 0, 0, 0};

    /** normal map テクスチャ (非所有、SetNormalMap で差し替え)。 */
    IRhiTexture* m_NormalMap = nullptr;

    /** SSAO visibility テクスチャ (非所有、SetSsao で差し替え)。 */
    IRhiTexture* m_SsaoTex     = nullptr;

    /** SSAO 強度 (0=neutral、1=通常、>1=強調)。 */
    f32          m_SsaoIntensity = 0.0f;

    /** SSAO サンプル用 1 / viewport 幅 (0 で SSAO 無効)。 */
    f32          m_SsaoInvW   = 0.0f;

    /** SSAO サンプル用 1 / viewport 高さ (0 で SSAO 無効)。 */
    f32          m_SsaoInvH   = 0.0f;

    /** SSAO 無効時に bind する fallback (1x1 全 255 = visibility 1)。 */
    TUniquePtr<IRhiTexture> m_SsaoFb;

    /** SSGI color テクスチャ (非所有、SetSsgi で差し替え)。 */
    IRhiTexture* m_SsgiTex       = nullptr;

    /** SSGI 強度。 */
    f32          m_SsgiIntensity = 0.0f;

    /** SSGI 無効時に bind する fallback (1x1 R11G11B10F、初期値 0)。 */
    TUniquePtr<IRhiTexture> m_SsgiFb;

    /** SSR 反射テクスチャ (非所有、SetSsr で差し替え)。 */
    IRhiTexture* m_SsrTex       = nullptr;

    /** SSR 強度。 */
    f32          m_SsrIntensity = 0.0f;

    /** SSR 無効時に bind する fallback (1x1 RGBA8 黒、hit mask 0)。 */
    TUniquePtr<IRhiTexture> m_SsrFb;

    /** Aerial perspective camera-volume LUT (非所有、SetAerialPerspective で差し替え)。 */
    IRhiTexture* m_ApVol = nullptr;

    /** AP パラメータ (x=enabled、y=max_dist scene)。 */
    FVec4 m_ApParams = FVec4{0, 0, 0, 0};

    /** AP 無効時に bind する fallback (1x1x1 RGBA16F、in-scatter 0 / opacity 0)。 */
    TUniquePtr<IRhiTexture> m_ApFb;

    /** lightmap テクスチャ (非所有、SetLightmap で差し替え)。 */
    IRhiTexture* m_LightmapTex       = nullptr;

    /** lightmap 強度。 */
    f32          m_LightmapIntensity = 0.0f;

    /** lightmap 無効時に bind する fallback (1x1 RGBA8 黒)。 */
    TUniquePtr<IRhiTexture> m_LightmapFb;

    /** shadow cascade の最大数 (FShadowMap::kMaxCascades と一致、frame CB の shadow_view_proj[4] と対応)。 */
    static constexpr u32 kMaxShadowCascades = 4;

    /** shadow map / cascade atlas の深度テクスチャ (非所有)。 */
    IRhiTexture* m_ShadowDepth          = nullptr;

    /** 各 cascade の light VP。 */
    FMat4         m_ShadowViewProj[kMaxShadowCascades] = {};

    /** shadow パラメータ (x=bias、y=enabled、z=texel_size、w=filter_radius)。 */
    FVec4         m_ShadowParams         = FVec4{0.002f, 0, 1.0f/2048.0f, 0};

    /** 各 cascade の view-space z far (single mode は全成分 inf で常に cascade 0)。 */
    FVec4         m_CascadeSplits        = FVec4{1e30f, 1e30f, 1e30f, 1e30f};

    /** cascade atlas の UV スケール (single mode は変換無しの {1,1})。 */
    FVec4         m_CascadeUvScale      = FVec4{1.0f, 1.0f, 0, 0};

    /** shadow 未バインド時に bind する fallback (1x1 depth-ish texture)。 */
    TUniquePtr<IRhiTexture> m_ShadowFb;

    /** 拡張 material パラメータ (x=clearcoat、y=coat_roughness、z=anisotropy)。 */
    FVec4         m_ExtParams     = FVec4{0, 0.5f, 0, 0};

    /** anisotropic 主軸の world tangent (xyz)。 */
    FVec4         m_AnisoTangent  = FVec4{1, 0, 0, 0};

    /** emissive (xyz=color*strength、既定 0 で無効)。 */
    FVec4         m_Emissive       = FVec4{0, 0, 0, 0};

    /** sheen パラメータ (xyz=color、w=weight。既定 0=OFF)。 */
    FVec4         m_SheenParams   = FVec4{0, 0, 0, 0};

    /** sheen の Charlie roughness (x=roughness)。 */
    FVec4         m_SheenRough    = FVec4{0.3f, 0, 0, 0};

    /** iridescence パラメータ (x=weight 既定 0=OFF、y=thickness(nm)、z=film IOR)。 */
    FVec4         m_IridParams    = FVec4{0, 400.0f, 1.4f, 0};

    /** subsurface パラメータ (xyz=color、w=weight。既定 0=OFF)。 */
    FVec4         m_SssParams     = FVec4{0, 0, 0, 0};
};

} // namespace acs
