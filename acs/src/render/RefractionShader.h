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
//   3. RefractionShader で屈折オブジェクトを HDR RT へ描画する (background を sample)
//
// 使い方:
//   RefractionShader refr;
//   refr.Init(*device, hdr_format, depth_format);
//   refr.SetFrame(camera.ViewProjection(), camera.Eye());
//   refr.DrawMesh(*cl, glass_mesh, model, background_tex, env_cubemap,
//                 /*ior=*/1.5f, /*thickness=*/0.5f, /*tint=*/Vec3{1,1,1});
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

class RefractionShader {
public:
    RefractionShader() noexcept = default;
    ~RefractionShader() noexcept = default;

    RefractionShader(const RefractionShader&)            = delete;
    RefractionShader& operator=(const RefractionShader&) = delete;

    // 初期化 (VS+PS コンパイル + パイプライン + 定数バッファ)。
    //   rt_format    : 描画先 RT のフォーマット (通常 HDR = R16G16B16A16_Float)
    //   depth_format : 深度フォーマット (opaque pass と同じものを渡す)
    Result<void> Init(IRhiDevice& device,
                      Format rt_format    = Format::B8G8R8A8_UNorm,
                      Format depth_format = Format::D32_Float) noexcept;

    // 全リソース解放
    void Shutdown() noexcept;

    // 毎フレーム呼ぶ (カメラの view-projection と world 空間の eye 位置)。
    void SetFrame(const Mat4& view_projection, Vec3 camera_pos) noexcept;

    // 描画オブジェクトごとに呼ぶ。
    //   ior       : 屈折率 (ガラス~1.5、水~1.33、ダイヤ~2.4)
    //   thickness : 屈折方向に背景を sample する距離 (大きいほど歪みが強い)
    //   tint      : ガラスの吸収色 (透明=白、色付きガラス=その色)
    //   roughness : 表面荒さ (Phase 35-3d、0=クリア、1=完全フロステッド)
    //   dispersion: 色収差/分散 (Phase 35-3e、0=色分離無し、1=強プリズム/ダイヤ風)
    void SetObject(const Mat4& model, f32 ior, f32 thickness, Vec3 tint,
                   f32 roughness = 0.0f, f32 dispersion = 0.0f) noexcept;

    IRhiPipeline* Pipeline()    const noexcept { return _pipeline.Get(); }
    IRhiBuffer*   PerFrameCB()  const noexcept { return _frame_cb.Get(); }
    IRhiBuffer*   PerObjectCB() const noexcept { return _object_cb.Get(); }

    // 1 関数で 1 体描画: SetObject + CB/Texture バインド + DrawIndexed をまとめる。
    //   background : 屈折で sample する opaque シーンの複製
    //   env        : Fresnel 反射に使う環境キューブマップ
    //   roughness  : 表面荒さ (Phase 35-3d、0=clear、>0 で多タップでブラー = frosted)
    //   dispersion : 色収差/分散 (Phase 35-3e、0=なし、>0 でプリズム/ダイヤ風の色分離)
    void DrawMesh(IRhiCommandList& cmd, const GpuMesh& mesh, const Mat4& model,
                  IRhiTexture& background, IRhiTexture& env,
                  f32 ior = 1.5f, f32 thickness = 0.5f,
                  Vec3 tint = Vec3{1, 1, 1},
                  f32 roughness = 0.0f,
                  f32 dispersion = 0.0f) noexcept;

private:
    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiPipeline> _pipeline;
    UniquePtr<IRhiBuffer>   _frame_cb;
    UniquePtr<IRhiBuffer>   _object_cb;
};

} // namespace acs
