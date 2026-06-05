// SPDX-License-Identifier: Apache-2.0
// スキンメッシュ用ライティングシェーダ（GPU スキニング）
//
// FStandardShader の上位互換: PerFrame (b0) / PerObject (b1) は同じレイアウト。
// 加えて Bones (b2) を持ち、最大 64 ボーンのパレット行列をシェーダに送る。
//
// 使い方:
//   FSkinnedShader shd;
//   shd.Init(*dev, color_fmt, depth_fmt);
//
//   // フレーム共通（FStandardShader と同じ呼び方）
//   shd.SetLights(camera.ViewProjection(), camera.Eye(),
//                 lights, count, ambient);
//
//   // オブジェクト
//   shd.SetObject(model_mat, base_color, specular, shininess);
//
//   // ボーンパレット（毎フレーム）
//   FMat4 palette[64];
//   u32 nb = anim_player.WritePalette(palette, 64);
//   shd.SetBonePalette(palette, nb);
//
//   cl->SetPipeline(*shd.Pipeline());
//   cl->SetConstantBuffer(0, *shd.PerFrameCB());
//   cl->SetConstantBuffer(1, *shd.PerObjectCB());
//   cl->SetConstantBuffer(2, *shd.BonesCB());
//   cl->SetTexture(0, *shd.DefaultWhiteTexture());
//   cl->SetVertexBuffer(*gm.vertex_buffer, gm.vertex_stride);
//   cl->SetIndexBuffer (*gm.index_buffer);
//   cl->DrawIndexed(gm.index_count);
#pragma once

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
#include "render/StandardShader.h"   // DirLight 共有

namespace acs {

/**
 * GPU スキニング対応のライティングシェーダ。
 *
 * @details
 * FStandardShader の上位互換で、PerFrame (b0) / PerObject (b1) は同レイアウト。
 * 加えて Bones (b2) を持ち、最大 kMaxBones 個のボーンパレット行列をシェーダへ送る。
 * 頂点シェーダで BLENDINDICES / BLENDWEIGHT を使い 4 ボーンを加重平均してスキニングし、
 * ピクセルシェーダで方向光 + 点光源の Blinn-Phong ライティングを計算する。
 */
class FSkinnedShader {
public:
    /** ボーンパレットの最大数 (シェーダ側 ACS_MAX_BONES と一致)。 */
    static constexpr u32 kMaxBones = 64;

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FSkinnedShader() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FSkinnedShader() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FSkinnedShader(const FSkinnedShader&)            = delete;

    /** コピー代入も禁止。 */
    FSkinnedShader& operator=(const FSkinnedShader&) = delete;

    /**
     * シェーダ・パイプライン・定数バッファ・既定白テクスチャを生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param rt_format レンダーターゲットのフォーマット。
     * @param depth_format 深度バッファのフォーマット (Unknown で深度テスト無効)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * 単一方向光でフレーム共通の状態を設定する (SetLights の簡易版)。
     *
     * @param view_projection view * projection 行列。
     * @param camera_pos カメラの world 位置 (スペキュラ計算に使う)。
     * @param light_dir 方向光の向き。
     * @param light_color 方向光の色。
     * @param ambient_color 環境光の色。
     */
    void SetFrame(const FMat4& view_projection,
                  FVec3 camera_pos,
                  FVec3 light_dir, FVec3 light_color,
                  FVec3 ambient_color) noexcept;

    /**
     * 複数の方向光でフレーム共通の状態を設定し、PerFrame CB を書き込む。
     *
     * @details count が最大数 (4) を超える場合は内部でクランプする。
     * @param view_projection view * projection 行列。
     * @param camera_pos カメラの world 位置 (スペキュラ計算に使う)。
     * @param lights 方向光の配列。
     * @param count 方向光の数 (最大 4)。
     * @param ambient_color 環境光の色。
     */
    void SetLights(const FMat4& view_projection,
                   FVec3 camera_pos,
                   const FDirLight* lights, u32 count,
                   FVec3 ambient_color) noexcept;

    /**
     * 点光源を設定し、PerFrame CB を再書き込みする。
     *
     * @details count が最大数 (4) を超える場合は内部でクランプする。
     * @param lights 点光源の配列。
     * @param count 点光源の数 (最大 4)。
     */
    void SetPointLights(const PointLight* lights, u32 count) noexcept;

    /**
     * 描画オブジェクトのモデル行列とマテリアルを設定する。
     *
     * @param model モデル (world) 行列。
     * @param base_color ベースカラー (アルベドへの乗算色)。
     * @param specular_strength スペキュラ強度。
     * @param shininess スペキュラの鋭さ (Blinn-Phong の指数)。
     */
    void SetObject(const FMat4& model,
                   FVec3 base_color = FVec3{1, 1, 1},
                   f32  specular_strength = 0.0f,
                   f32  shininess = 32.0f) noexcept;

    /**
     * ボーンパレット行列を設定する。
     *
     * @details count が kMaxBones を超える場合はクランプし、残りは単位行列で埋める。
     * @param palette ボーン行列の配列。
     * @param count パレットの行列数 (最大 kMaxBones)。
     */
    void SetBonePalette(const FMat4* palette, u32 count) noexcept;

    /**
     * 描画パイプラインを返す。
     *
     * @return スキニング描画用パイプライン。
     */
    IRhiPipeline*  Pipeline()    const noexcept { return m_Pipeline.Get(); }

    /**
     * PerFrame 定数バッファ (b0) を返す。
     *
     * @return view_proj・ライト等を格納した定数バッファ。
     */
    IRhiBuffer*    PerFrameCB()  const noexcept { return m_FrameCb.Get(); }

    /**
     * PerObject 定数バッファ (b1) を返す。
     *
     * @return モデル行列・マテリアルを格納した定数バッファ。
     */
    IRhiBuffer*    PerObjectCB() const noexcept { return m_ObjectCb.Get(); }

    /**
     * Bones 定数バッファ (b2) を返す。
     *
     * @return ボーンパレット行列を格納した定数バッファ。
     */
    IRhiBuffer*    BonesCB()     const noexcept { return m_BonesCb.Get(); }

    /**
     * テクスチャ未指定時に使う 1x1 白テクスチャを返す。
     *
     * @return 既定の白テクスチャ (slot 0 に bind する)。
     */
    IRhiTexture*   DefaultWhiteTexture() const noexcept { return m_White.Get(); }

private:
    /** キャッシュした Frame 状態を PerFrame CB へ書き込む。 */
    void FlushFrameCB() noexcept;

    /** 頂点シェーダ (スキニング + world 変換)。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** ピクセルシェーダ (方向光 + 点光源ライティング)。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** スキニング描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** PerFrame 定数バッファ (b0)。 */
    TUniquePtr<IRhiBuffer>   m_FrameCb;

    /** PerObject 定数バッファ (b1)。 */
    TUniquePtr<IRhiBuffer>   m_ObjectCb;

    /** Bones 定数バッファ (b2)。 */
    TUniquePtr<IRhiBuffer>   m_BonesCb;

    /** テクスチャ未指定時の 1x1 白テクスチャ。 */
    TUniquePtr<IRhiTexture>  m_White;

    /** キャッシュした view * projection 行列。 */
    FMat4       m_Vp;

    /** キャッシュしたカメラ world 位置。 */
    FVec3       m_Eye = FVec3{0, 0, 0};

    /** キャッシュした環境光の色。 */
    FVec3       m_Ambient = FVec3{0, 0, 0};

    /** キャッシュした方向光 (最大 4)。 */
    FDirLight   m_DirLights[4];

    /** 有効な方向光の数。 */
    u32        m_DirCount = 0;

    /** キャッシュした点光源 (最大 4)。 */
    PointLight m_PointLights[4];

    /** 有効な点光源の数。 */
    u32        m_PointCount = 0;
};

} // namespace acs
