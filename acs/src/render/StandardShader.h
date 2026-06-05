// SPDX-License-Identifier: Apache-2.0
// 標準ライティングシェーダ（最大 4 灯 + Blinn-Phong スペキュラ）
//
// 用途: メッシュアセット (位置 + 法線 + UV) を、複数の有向光源 +
//       環境光 + 鏡面反射 + アルベドテクスチャで描画する。
//
// 使い方（単一ライトのお手軽版）:
//   FStandardShader shd;
//   shd.Init(*renderer.Device(), renderer.ColorFormat(), renderer.DepthFormat());
//   shd.SetFrame(camera.ViewProjection(), camera.Eye(),
//                FVec3{-0.5f,-1,0.3f}, FVec3{1,1,1}, FVec3{0.1f,0.1f,0.15f});
//
// マルチライト版:
//   FDirLight lights[2];
//   lights[0].direction = FVec3{0.5f, -1, 0.3f}; lights[0].color = FVec3{1, 0.9f, 0.7f};
//   lights[1].direction = FVec3{-0.4f, -0.6f, -0.8f}; lights[1].color = FVec3{0.3f, 0.4f, 0.6f};
//   shd.SetLights(camera.ViewProjection(), camera.Eye(),
//                 lights, 2, FVec3{0.1f, 0.1f, 0.15f});
//
// 簡単版 (1 関数で 1 体描画):
//   shd.DrawMesh(*renderer.CommandList(), gm, model_mat,
//                FVec3{1,1,1}, 0.5f, 64.0f, /*albedo=*/nullptr);
//
// 細かい制御版 (オブジェクト CB を上書きしないとき等):
//   shd.SetObject(model_mat, FVec3{1,1,1}, /*specular_strength=*/0.5f, /*shininess=*/64.0f);
//   auto* cl = renderer.CommandList();
//   cl->SetPipeline(*shd.Pipeline());
//   cl->SetConstantBuffer(0, *shd.PerFrameCB());
//   cl->SetConstantBuffer(1, *shd.PerObjectCB());
//   cl->SetTexture(0, my_texture_or_shd.DefaultWhiteTexture());
//   cl->SetVertexBuffer(*gm.vertex_buffer, gm.vertex_stride);
//   cl->SetIndexBuffer (*gm.index_buffer);
//   cl->DrawIndexed(gm.index_count);
#pragma once

#include "render/RenderAssets.h"   // GpuMesh
#include "render/IRhiCommandList.h"

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
 * 1 灯ぶんの有向光源 (平行光源)。
 *
 * @details
 * direction はワールド空間の「光が向かう方向」。光源から見た方向の逆を渡しても
 * シェーダ側で normalize されるため問題ない。
 */
struct FDirLight {
    /** 光が向かう方向 (ワールド空間、シェーダ側で正規化される)。 */
    FVec3 direction = FVec3{0, -1, 0};

    /** 光の色 (例 (1,1,1))。 */
    FVec3 color     = FVec3{1, 1, 1};
};

/**
 * 点光源 (ワールド位置 + 到達距離付き)。
 */
struct PointLight {
    /** 光源のワールド位置。 */
    FVec3 position = FVec3{0, 0, 0};

    /** 到達距離。この距離を超えると影響はゼロになる。 */
    f32  range    = 10.0f;

    /** 光の色。 */
    FVec3 color    = FVec3{1, 1, 1};
};

/**
 * 標準ライティングシェーダ (最大 4 灯の平行光源 + 4 灯の点光源 + Blinn-Phong)。
 *
 * @details
 * メッシュ (位置 + 法線 + UV) を、複数の有向光源・点光源・環境光・鏡面反射・
 * アルベドテクスチャ・シャドウマップ (PCSS) で描画する。VS/PS とパイプライン・
 * 定数バッファ・1x1 白テクスチャを単独所有する。Frame の状態 (カメラ・各種ライト・
 * シャドウ) はメンバにキャッシュされ、SetLights / SetPointLights / SetShadowMap を
 * 独立に呼んでも整合する Frame CB へ反映される。
 */
class FStandardShader {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FStandardShader() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FStandardShader() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FStandardShader(const FStandardShader&)            = delete;

    /** コピー代入も禁止。 */
    FStandardShader& operator=(const FStandardShader&) = delete;

    /**
     * GPU リソースを確保する。
     *
     * @details VS/PS のコンパイル、パイプライン、Frame/Object 定数バッファ、
     * デフォルトの 1x1 白テクスチャをまとめて生成する。
     * @param device リソース生成に使う RHI デバイス。
     * @param rt_format 出力レンダーターゲットのフォーマット。
     * @param depth_format 深度バッファのフォーマット (Unknown で深度テスト無効)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * カメラ + 1 灯の有向光源 + 環境光で Frame CB を更新する (マルチライト不要時の簡易 API)。
     *
     * @details 内部的に SetLights を count=1 で呼ぶ。毎フレーム呼ぶ。
     * @param view_projection カメラの view-projection 行列。
     * @param camera_pos カメラのワールド位置 (鏡面反射の視線計算に使う)。
     * @param light_dir 光が向かう方向 (ワールド空間、正規化推奨)。
     * @param light_color 光の色 (例 (1,1,1))。
     * @param ambient_color 環境光の色 (例 (0.1, 0.1, 0.15))。
     */
    void SetFrame(const FMat4& view_projection,
                  FVec3 camera_pos,
                  FVec3 light_dir,
                  FVec3 light_color,
                  FVec3 ambient_color
                  ) noexcept;

    /**
     * カメラ + 複数の有向光源 + 環境光で Frame CB を更新する (最大 4 灯)。
     *
     * @details count が 4 を超える分は無視される。
     * @param view_projection カメラの view-projection 行列。
     * @param camera_pos カメラのワールド位置 (鏡面反射の視線計算に使う)。
     * @param lights 有向光源配列の先頭ポインタ。
     * @param count 有効な光源数 (4 を超える分は切り捨て)。
     * @param ambient_color 環境光の色。
     */
    void SetLights(const FMat4& view_projection,
                   FVec3 camera_pos,
                   const FDirLight* lights, u32 count,
                   FVec3 ambient_color) noexcept;

    /**
     * 点光源を最大 4 灯まで設定する (SetLights / SetFrame とは独立に追加適用)。
     *
     * @details 呼ばない、または count=0 のときは点光源なし。count が 4 を超える分は無視される。
     * @param lights 点光源配列の先頭ポインタ。
     * @param count 有効な点光源数 (4 を超える分は切り捨て)。
     */
    void SetPointLights(const PointLight* lights, u32 count) noexcept;

    /**
     * シャドウマップを設定する (PCSS ソフトシャドウ、最初の有向光源にのみ適用)。
     *
     * @details
     * tex には FShadowMap::DepthTexture()、light_vp には同 LightViewProjection() を渡す。
     * tex に nullptr を渡すとシャドウ無効 (描画は影なしで進む)。filter_radius は影の
     * 柔らかさスケールで、0=実質ハード影 (1 texel の min penumbra)、1.0=標準 PCSS
     * (Fernando 2005 の light_size=0.01 相当、FPbrShader と一致)、>1 でより柔らかい半影。
     * @param tex シャドウマップの深度テクスチャ (nullptr で無効化)。
     * @param light_vp ライト視点の view-projection 行列。
     * @param bias シャドウアクネ回避用バイアス (一般に 0.0005..0.005)。
     * @param filter_radius 半影スケール (0=ハード、1=標準 PCSS、>1=より柔らか)。
     */
    void SetShadowMap(IRhiTexture* tex, const FMat4& light_vp,
                      f32 bias = 0.001f, f32 filter_radius = 1.0f) noexcept;

    /**
     * シャドウが有効かを返す。
     *
     * @return シャドウマップが設定済みなら true。
     */
    bool IsShadowEnabled() const noexcept { return m_ShadowTex != nullptr; }

    /**
     * シャドウマップを返す (未設定なら白テクスチャ)。
     *
     * @return 設定済みのシャドウマップ、なければデフォルト白テクスチャ。
     */
    IRhiTexture* ShadowTextureOrDefault() const noexcept {
        return m_ShadowTex ? m_ShadowTex : m_White.Get();
    }

    /**
     * 描画オブジェクトごとに Object CB を更新する (モデル行列・色・材質)。
     *
     * @param model モデル行列 (ローカル→ワールド)。
     * @param base_color ベースカラー (アルベドテクスチャに乗算)。
     * @param specular_strength 鏡面反射の強さ (0=完全マット、1=強いハイライト)。
     * @param shininess 鏡面のシャープさ (8=広い反射、128=シャープなハイライト)。
     */
    void SetObject(const FMat4& model,
                   FVec3 base_color         = FVec3{1, 1, 1},
                   f32  specular_strength  = 0.0f,
                   f32  shininess          = 32.0f) noexcept;

    /**
     * 描画パイプラインを返す。
     *
     * @return Init で生成したパイプライン。
     */
    IRhiPipeline*  Pipeline()      const noexcept { return m_Pipeline.Get(); }

    /**
     * Frame 定数バッファ (b0) を返す。
     *
     * @return Frame 定数バッファ。
     */
    IRhiBuffer*    PerFrameCB()    const noexcept { return m_FrameCb.Get(); }

    /**
     * Object 定数バッファ (b1) を返す。
     *
     * @return Object 定数バッファ。
     */
    IRhiBuffer*    PerObjectCB()   const noexcept { return m_ObjectCb.Get(); }

    /**
     * デフォルトの 1x1 白テクスチャを返す (テクスチャを指定したくないとき用)。
     *
     * @return 1x1 白テクスチャ。
     */
    IRhiTexture*   DefaultWhiteTexture() const noexcept { return m_White.Get(); }

    /**
     * 1 関数でメッシュを 1 体描画する。
     *
     * @details
     * SetObject による Object CB 更新 + SetPipeline + 定数バッファ・テクスチャ・
     * 頂点/インデックスバッファのバインド + DrawIndexed をまとめて発行する。Frame CB は
     * 事前に SetLights 等で設定しておく。細かく制御したい場合は Pipeline() /
     * PerFrameCB() / PerObjectCB() を直接使う。
     * @param cmd コマンドを積むコマンドリスト。
     * @param mesh 描画する GPU メッシュ (頂点/インデックスバッファ)。
     * @param model モデル行列 (ローカル→ワールド)。
     * @param base_color ベースカラー (アルベドに乗算)。
     * @param specular_strength 鏡面反射の強さ。
     * @param shininess 鏡面のシャープさ。
     * @param albedo アルベドテクスチャ (nullptr でデフォルト白テクスチャを使用)。
     */
    void DrawMesh(class IRhiCommandList& cmd,
                  const struct GpuMesh& mesh,
                  const FMat4& model,
                  FVec3 base_color        = FVec3{1, 1, 1},
                  f32  specular_strength = 0.0f,
                  f32  shininess         = 32.0f,
                  IRhiTexture* albedo    = nullptr) noexcept;

private:
    /** キャッシュ済みの Frame 状態を Frame 定数バッファへ書き込む。 */
    void FlushFrameCB() noexcept;

    /** 頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** ピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** 描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** Frame 定数バッファ (b0)。 */
    TUniquePtr<IRhiBuffer>   m_FrameCb;

    /** Object 定数バッファ (b1)。 */
    TUniquePtr<IRhiBuffer>   m_ObjectCb;

    /** デフォルトの 1x1 白テクスチャ。 */
    TUniquePtr<IRhiTexture>  m_White;

    /** カメラの view-projection 行列 (Frame 状態キャッシュ)。 */
    FMat4       m_Vp;

    /** カメラのワールド位置。 */
    FVec3       m_Eye      = FVec3{0, 0, 0};

    /** 環境光の色。 */
    FVec3       m_Ambient  = FVec3{0, 0, 0};

    /** 有向光源の配列 (最大 4 灯)。 */
    FDirLight   m_DirLights[4];

    /** 有効な有向光源数。 */
    u32        m_DirCount = 0;

    /** 点光源の配列 (最大 4 灯)。 */
    PointLight m_PointLights[4];

    /** 有効な点光源数。 */
    u32        m_PointCount = 0;

    /** ライト視点の view-projection 行列 (シャドウ投影用)。 */
    FMat4       m_LightVp;

    /** シャドウバイアス (アクネ回避)。 */
    f32        m_ShadowBias = 0.001f;

    /** PCSS の半影スケール (shadow_params.w)。 */
    f32        m_ShadowFilter = 1.0f;

    /** シャドウマップの深度テクスチャ (弱参照、所有しない)。 */
    IRhiTexture* m_ShadowTex = nullptr;
};

} // namespace acs
