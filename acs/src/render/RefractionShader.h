// SPDX-License-Identifier: Apache-2.0
// スクリーンスペース屈折シェーダ (Phase 3)
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

class FRefractionShader {
public:
    FRefractionShader() noexcept = default;
    ~FRefractionShader() noexcept = default;

    FRefractionShader(const FRefractionShader&)            = delete;
    FRefractionShader& operator=(const FRefractionShader&) = delete;

    // 初期化 (VS+PS コンパイル + パイプライン + 定数バッファ)。
    //   rt_format    : 描画先 RT のフォーマット (通常 HDR = R16G16B16A16_Float)
    //   depth_format : 深度フォーマット (opaque pass と同じものを渡す)
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;

    // 全リソース解放
    void Shutdown() noexcept;

    // 毎フレーム呼ぶ (カメラの view-projection と world 空間の eye 位置)。
    void SetFrame(const FMat4& view_projection, FVec3 camera_pos) noexcept;

    // Phase 35-3f thickness map: 背面 (cull=Front で描いた) 深度を渡すと、
    // 各 pixel の表面/背面 view-space z 差から実厚みを計算し、Beer-Lambert
    // 吸収 (tint^thickness) と屈折オフセットの両方で使う。スカラー thickness
    // (SetObject の引数) は back_depth が無いか enabled=0 のときの fallback。
    //
    //   back_depth : 透明オブジェクトの背面深度を焼いた D32_Float
    //                (shader_visible_depth=true)。null で従来のスカラー厚みに戻る。
    //   near / far : カメラの透視 near/far。NDC depth → view-space z 逆変換に使う。
    //   screen_w / screen_h: 主パス HDR RT の解像度 (SV_POSITION から UV を作るため)。
    void SetBackDepth(IRhiTexture* back_depth, f32 near, f32 far,
                       u32 screen_w, u32 screen_h) noexcept;

    // 描画オブジェクトごとに呼ぶ。
    //   ior       : 屈折率 (ガラス~1.5、水~1.33、ダイヤ~2.4)
    //   thickness : 屈折方向に背景を sample する距離 (大きいほど歪みが強い)。
    //                Phase 35-3f で back_depth がセットされていれば per-pixel
    //                計測値が優先され、本引数は fallback として残る。
    //   tint      : ガラスの吸収色 (透明=白、色付きガラス=その色)
    //   roughness : 表面荒さ (Phase 35-3d、0=クリア、1=完全フロステッド)
    //   dispersion: 色収差/分散 (Phase 35-3e、0=色分離無し、1=強プリズム/ダイヤ風)
    void SetObject(const FMat4& model, f32 ior, f32 thickness, FVec3 tint,
                   f32 roughness = 0.0f, f32 dispersion = 0.0f) noexcept;

    IRhiPipeline* Pipeline()    const noexcept { return m_Pipeline.Get(); }
    IRhiBuffer*   PerFrameCB()  const noexcept { return m_FrameCb.Get(); }
    IRhiBuffer*   PerObjectCB() const noexcept { return m_ObjectCb.Get(); }

    // 1 関数で 1 体描画: SetObject + CB/Texture バインド + DrawIndexed をまとめる。
    //   background : 屈折で sample する opaque シーンの複製
    //   env        : Fresnel 反射に使う環境キューブマップ
    //   roughness  : 表面荒さ (Phase 35-3d、0=clear、>0 で多タップでブラー = frosted)
    //   dispersion : 色収差/分散 (Phase 35-3e、0=なし、>0 でプリズム/ダイヤ風の色分離)
    void DrawMesh(IRhiCommandList& cmd, const GpuMesh& mesh, const FMat4& model,
                  IRhiTexture& background, IRhiTexture& env,
                  f32 ior = 1.5f, f32 thickness = 0.5f,
                  FVec3 tint = FVec3{1, 1, 1},
                  f32 roughness = 0.0f,
                  f32 dispersion = 0.0f) noexcept;

private:
    TUniquePtr<IRhiShader>   m_Vs;
    TUniquePtr<IRhiShader>   m_Ps;
    TUniquePtr<IRhiPipeline> m_Pipeline;
    TUniquePtr<IRhiBuffer>   m_FrameCb;
    TUniquePtr<IRhiBuffer>   m_ObjectCb;

    // Phase 35-3f thickness map
    IRhiTexture*           m_BackDepth   = nullptr;     // 弱参照 (caller owns)
    TUniquePtr<IRhiTexture> m_BackDepthFb;              // 1x1 R32F fallback (enabled=0 用)
    f32                    m_BackNear    = 0.1f;
    f32                    m_BackFar     = 100.0f;
    u32                    m_ScreenW     = 1;
    u32                    m_ScreenH     = 1;
    bool                   m_BackEnabled = false;
    // SetFrame で view_proj / camera_pos を覚えておく (SetBackDepth が screen
    // params だけ更新する際に同じ Frame CB を再書き込みする必要がある)。
    FMat4                   m_Vp           = {};
    FVec3                   m_Eye          = FVec3{0, 0, 0};
};

} // namespace acs
