// SPDX-License-Identifier: Apache-2.0
// スクリーンスペース屈折シェーダ
//
// 用途: ガラス・水・氷のような透明屈折オブジェクトを描画する。屈折は
//       「描画済みの opaque シーン (background) を、屈折方向にずらして
//       sample する」スクリーンスペース手法で表現する。
//
// 前提となるフレーム構成 (呼び出し側 = サンプルが用意する):
//   1. opaque ジオメトリを HDR RT へ描画する
//   2. HDR RT を background テクスチャへ複製する (屈折オブジェクトが読むため。
//      同一 RT の read+write は不可なので複製が要る)
//   3. FRefractionShader で屈折オブジェクトを HDR RT へ描画する (background を sample)
//
// 使い方:
//   FRefractionShader refr;
//   refr.Init(*device, hdr_format, depth_format);
//   refr.SetFrame(camera.ViewProjection(), camera.Eye());
//   refr.DrawMesh(*cl, glass_mesh, model, background_tex, env_cubemap,
//                 /*ior=*/1.5f, /*thickness=*/0.5f, /*tint=*/FVec3{1,1,1});
//
// blend は Opaque。屈折オブジェクトは「背景を曲げた色」を不透明に書き込むため
// alpha blend は不要 (深度も書き、後続の描画を正しく遮蔽する)。
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
 * スクリーンスペース屈折シェーダ (ガラス・水・氷)。
 *
 * @details
 * 描画済みの opaque シーン (background) を屈折方向へずらして sample することで透明屈折
 * オブジェクトを表現する。blend は Opaque で深度も書き、後続の描画を正しく遮蔽する。
 * GPU リソースを単独所有する non-copy 型。
 */
class FRefractionShader {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FRefractionShader() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FRefractionShader() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FRefractionShader(const FRefractionShader&)            = delete;

    /** コピー代入も禁止。 */
    FRefractionShader& operator=(const FRefractionShader&) = delete;

    /**
     * VS+PS のコンパイル・パイプライン・定数バッファを生成する。
     *
     * @param device リソース・パイプライン生成に使う RHI デバイス。
     * @param rt_format 描画先 RT のフォーマット (通常 HDR = R16G16B16A16_Float)。
     * @param depth_format 深度フォーマット (opaque pass と同じものを渡す)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;

    /** 確保した全リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * 毎フレームのカメラ情報を設定する。
     *
     * @param view_projection カメラの view-projection 行列。
     * @param camera_pos world 空間の eye 位置。
     */
    void SetFrame(const FMat4& view_projection, FVec3 camera_pos) noexcept;

    /**
     * per-pixel 厚み計測用に背面深度マップを設定する。
     *
     * @details
     * 背面 (cull=Front で描いた) 深度を渡すと、各 pixel の表面/背面 view-space z 差から
     * 実厚みを計算し、Beer-Lambert 吸収 (tint^thickness) と屈折オフセットの両方で使う。
     * SetObject のスカラー thickness は back_depth が無いか enabled=0 のときの fallback。
     * @param back_depth 透明オブジェクトの背面深度を焼いた D32_Float (shader_visible_depth=true)。null で従来のスカラー厚みに戻る。
     * @param near カメラの透視 near (NDC depth → view-space z 逆変換に使う)。
     * @param far カメラの透視 far (NDC depth → view-space z 逆変換に使う)。
     * @param screen_w 主パス HDR RT の幅 (SV_POSITION から UV を作るため)。
     * @param screen_h 主パス HDR RT の高さ。
     */
    void SetBackDepth(IRhiTexture* back_depth, f32 near, f32 far,
                       u32 screen_w, u32 screen_h) noexcept;

    /**
     * 描画オブジェクトごとのマテリアルを設定する。
     *
     * @param model オブジェクトの world 変換行列。
     * @param ior 屈折率 (ガラス~1.5、水~1.33、ダイヤ~2.4)。
     * @param thickness 屈折方向に背景を sample する距離 (大きいほど歪みが強い)。back_depth がセット済みなら per-pixel 計測値が優先され、本引数は fallback。
     * @param tint ガラスの吸収色 (透明=白、色付きガラス=その色)。
     * @param roughness 表面荒さ (0=クリア、1=完全フロステッド)。
     * @param dispersion 色収差/分散 (0=色分離無し、1=強プリズム/ダイヤ風)。
     */
    void SetObject(const FMat4& model, f32 ior, f32 thickness, FVec3 tint,
                   f32 roughness = 0.0f, f32 dispersion = 0.0f) noexcept;

    /**
     * 屈折描画パイプラインを返す。
     *
     * @return 屈折描画のパイプライン。
     */
    IRhiPipeline* Pipeline()    const noexcept { return m_Pipeline.Get(); }

    /**
     * per-frame 定数バッファを返す。
     *
     * @return view-projection / eye 位置などを格納する CB。
     */
    IRhiBuffer*   PerFrameCB()  const noexcept { return m_FrameCb.Get(); }

    /**
     * per-object 定数バッファを返す。
     *
     * @return model 行列やマテリアルを格納する CB。
     */
    IRhiBuffer*   PerObjectCB() const noexcept { return m_ObjectCb.Get(); }

    /**
     * 1 関数で 1 体描画する (SetObject + CB/Texture バインド + DrawIndexed をまとめる)。
     *
     * @param cmd コマンドを積むコマンドリスト。
     * @param mesh 描画する屈折メッシュ。
     * @param model オブジェクトの world 変換行列。
     * @param background 屈折で sample する opaque シーンの複製。
     * @param env Fresnel 反射に使う環境キューブマップ。
     * @param ior 屈折率 (ガラス~1.5、水~1.33、ダイヤ~2.4)。
     * @param thickness 屈折方向に背景を sample する距離 (大きいほど歪みが強い)。
     * @param tint ガラスの吸収色 (透明=白、色付きガラス=その色)。
     * @param roughness 表面荒さ (0=clear、>0 で多タップでブラー = frosted)。
     * @param dispersion 色収差/分散 (0=なし、>0 でプリズム/ダイヤ風の色分離)。
     */
    void DrawMesh(IRhiCommandList& cmd, const GpuMesh& mesh, const FMat4& model,
                  IRhiTexture& background, IRhiTexture& env,
                  f32 ior = 1.5f, f32 thickness = 0.5f,
                  FVec3 tint = FVec3{1, 1, 1},
                  f32 roughness = 0.0f,
                  f32 dispersion = 0.0f) noexcept;

private:
    /** 屈折オブジェクトの頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** 背景を屈折 sample するピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** 屈折描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** per-frame 定数バッファ (view-projection / eye)。 */
    TUniquePtr<IRhiBuffer>   m_FrameCb;

    /** per-object 定数バッファ (model / マテリアル)。 */
    TUniquePtr<IRhiBuffer>   m_ObjectCb;

    /** per-pixel 厚み計測用の背面深度マップ (弱参照、caller owns)。 */
    IRhiTexture*           m_BackDepth   = nullptr;

    /** enabled=0 用の 1x1 R32F fallback 深度テクスチャ。 */
    TUniquePtr<IRhiTexture> m_BackDepthFb;

    /** 背面深度を view-space z へ戻す際の near。 */
    f32                    m_BackNear    = 0.1f;

    /** 背面深度を view-space z へ戻す際の far。 */
    f32                    m_BackFar     = 100.0f;

    /** 主パス HDR RT の幅 (SV_POSITION → UV 用)。 */
    u32                    m_ScreenW     = 1;

    /** 主パス HDR RT の高さ。 */
    u32                    m_ScreenH     = 1;

    /** 背面深度マップが有効か (false なら fallback スカラー厚み)。 */
    bool                   m_bBackEnabled = false;

    /**
     * SetFrame で受け取った view-projection 行列を保持する。
     *
     * @details SetBackDepth が screen params だけ更新する際に同じ Frame CB を再書き込みする
     * ために覚えておく。
     */
    FMat4                   m_Vp           = {};

    /** SetFrame で受け取った world 空間の eye 位置 (Frame CB 再書き込み用)。 */
    FVec3                   m_Eye          = FVec3{0, 0, 0};
};

} // namespace acs
