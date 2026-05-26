// SPDX-License-Identifier: Apache-2.0
// 標準ライティングシェーダ（最大 4 灯 + Blinn-Phong スペキュラ）
//
// 用途: メッシュアセット (位置 + 法線 + UV) を、複数の有向光源 +
//       環境光 + 鏡面反射 + アルベドテクスチャで描画する。
//
// 使い方（単一ライトのお手軽版）:
//   StandardShader shd;
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

// 1 灯ぶんの有向光源
struct FDirLight {
    FVec3 direction = FVec3{0, -1, 0};   // ワールド空間の「光が向かう方向」（光源から見た方向の逆でも可、シェーダ側で normalize）
    FVec3 color     = FVec3{1, 1, 1};
};

// 点光源（ワールド位置 + 到達距離）
struct PointLight {
    FVec3 position = FVec3{0, 0, 0};
    f32  range    = 10.0f;             // この距離を超えると影響ゼロ
    FVec3 color    = FVec3{1, 1, 1};
};

class StandardShader {
public:
    StandardShader() noexcept = default;
    ~StandardShader() noexcept = default;

    StandardShader(const StandardShader&)            = delete;
    StandardShader& operator=(const StandardShader&) = delete;

    // 初期化（VS+PS コンパイル + パイプライン + 定数バッファ + デフォルト白テクスチャ）
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;

    // 全リソース解放
    void Shutdown() noexcept;

    // 毎フレーム呼ぶ（カメラ + 1 灯の有向光源 + 環境光）。マルチライト不要なときの簡易 API。
    void SetFrame(const FMat4& view_projection,
                  FVec3 camera_pos,
                  FVec3 light_dir,    // ワールド空間、正規化推奨
                  FVec3 light_color,  // 例 (1,1,1)
                  FVec3 ambient_color // 例 (0.1, 0.1, 0.15)
                  ) noexcept;

    // マルチライト版（最大 4 灯）
    void SetLights(const FMat4& view_projection,
                   FVec3 camera_pos,
                   const FDirLight* lights, u32 count,
                   FVec3 ambient_color) noexcept;

    // 点光源を最大 4 灯まで設定（SetLights / SetFrame と独立、追加で適用される）
    // 呼ばないか count=0 なら点光源無し。
    void SetPointLights(const PointLight* lights, u32 count) noexcept;

    // シャドウマップ設定。tex は ShadowMap::DepthTexture()、light_vp は同 LightViewProjection()。
    // tex に nullptr を渡すとシャドウ無効化（描画は影なしで進む）。
    // bias はシャドウアクネ回避用（一般に 0.0005..0.005）。
    //
    // Phase 36-2 PCSS: filter_radius は影の柔らかさスケール。
    //   0    = hard PCF (penumbra 計算しても min(=1 texel) で実質ハード影)
    //   1.0  = 標準 PCSS (Fernando 2005 light_size=0.01 相当、PbrShader と一致)
    //   >1   = より柔らかい penumbra (面光源を大きく見せたいとき)
    void SetShadowMap(IRhiTexture* tex, const FMat4& light_vp,
                      f32 bias = 0.001f, f32 filter_radius = 1.0f) noexcept;
    bool IsShadowEnabled() const noexcept { return _shadow_tex != nullptr; }
    IRhiTexture* ShadowTextureOrDefault() const noexcept {
        return _shadow_tex ? _shadow_tex : _white.Get();
    }

    // 描画オブジェクトごとに呼ぶ
    // specular_strength: 0=完全マット、1=強いハイライト
    // shininess        : 8=広い反射、128=シャープなハイライト
    void SetObject(const FMat4& model,
                   FVec3 base_color         = FVec3{1, 1, 1},
                   f32  specular_strength  = 0.0f,
                   f32  shininess          = 32.0f) noexcept;

    IRhiPipeline*  Pipeline()      const noexcept { return _pipeline.Get(); }
    IRhiBuffer*    PerFrameCB()    const noexcept { return _frame_cb.Get(); }
    IRhiBuffer*    PerObjectCB()   const noexcept { return _object_cb.Get(); }

    // テクスチャを指定したくない場合に渡せる 1×1 白テクスチャ
    IRhiTexture*   DefaultWhiteTexture() const noexcept { return _white.Get(); }

    // 1 関数で描画 1 体分: SetPipeline + CB + Texture + VB/IB + DrawIndexed をまとめる。
    // 細かく制御したい場合は Pipeline()/PerFrameCB()/PerObjectCB() を直接使う。
    //
    //   shd.SetLights(...) は事前に呼んでおく (Frame CB 設定)。
    //   この DrawMesh は Object CB の更新 + 1 回の描画コマンド発行を行う。
    //
    //   albedo = nullptr で DefaultWhiteTexture が使われる。
    void DrawMesh(class IRhiCommandList& cmd,
                  const struct GpuMesh& mesh,
                  const FMat4& model,
                  FVec3 base_color        = FVec3{1, 1, 1},
                  f32  specular_strength = 0.0f,
                  f32  shininess         = 32.0f,
                  IRhiTexture* albedo    = nullptr) noexcept;

private:
    void FlushFrameCB() noexcept;

    TUniquePtr<IRhiShader>   _vs;
    TUniquePtr<IRhiShader>   _ps;
    TUniquePtr<IRhiPipeline> _pipeline;
    TUniquePtr<IRhiBuffer>   _frame_cb;
    TUniquePtr<IRhiBuffer>   _object_cb;
    TUniquePtr<IRhiTexture>  _white;

    // Frame の状態キャッシュ（SetLights / SetPointLights が独立に呼べるように）
    FMat4       _vp;
    FVec3       _eye      = FVec3{0, 0, 0};
    FVec3       _ambient  = FVec3{0, 0, 0};
    FDirLight   _dir_lights[4];
    u32        _dir_count = 0;
    PointLight _point_lights[4];
    u32        _point_count = 0;
    FMat4       _light_vp;
    f32        _shadow_bias = 0.001f;
    f32        _shadow_filter = 1.0f;       // Phase 36-2 PCSS: filter_radius (w of shadow_params)
    IRhiTexture* _shadow_tex = nullptr;     // 弱参照（user owns）
};

} // namespace acs
