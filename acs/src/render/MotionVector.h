// SPDX-License-Identifier: Apache-2.0
// motion + normal G-buffer の geometry pass (Phase 34f-3 / 34m)
//
// シーンの全 mesh を再ラスタライズし、MRT 2 枚を書き出す:
//   - motion (RG16F)  : screen-space motion vector (prev_uv - curr_uv)。camera
//                       動きと object 動きの両方を含む (前フレームの model + VP)。
//                       TAA が history を正確に reproject し ghost / trail を消す。
//   - normal (RGBA16F): world-space normal。頂点法線をピクセル補間したもので、
//                       depth-derivative の cross(ddx,ddy) と違い曲面でも段差が
//                       出ない。SSR/SSGI/SSAO がこれを sample し、faceted な
//                       反射ベクトル由来のジャギーを根本解消する (Phase 34m)。
//
// Phase 34f-2 までの TAA は depth からの camera reprojection のみ対応で、
// 動く mesh は ghost していた。本モジュールがその穴を埋める。
//
// 設計:
//   - FShadowMap と同じ Begin/DrawMesh/End パターン (caller がシーンを描く)
//   - 全 mesh を描く前提 (静的 mesh は prev_model == model)。motion texture は
//     画面全体で authoritative になり、TAA は depth を併用せず済む
//     (→ TAA resolve PSO の texture slot を増やさず slot binding 問題を回避)
//   - occlusion 用に専用 depth buffer を内部に持つ (scene depth は共有しない)
//
// 使い方:
//   FMotionVector mv;
//   mv.Init(*dev, w, h);
//   ...毎フレーム (シーン color pass のあと):
//   mv.Begin(*cl, vp_no_jitter, prev_vp_no_jitter);
//   for (each mesh) mv.DrawMesh(*cl, gm, curr_model, prev_model);
//   mv.End(*cl);
//   post_params.taa_motion_texture = mv.OutputTexture();
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"
#include "render/RenderAssets.h"        // GpuMesh
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"

namespace acs {

class FMotionVector {
public:
    FMotionVector() noexcept = default;
    ~FMotionVector() noexcept = default;

    FMotionVector(const FMotionVector&)            = delete;
    FMotionVector& operator=(const FMotionVector&) = delete;

    TResult<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;
    void Shutdown() noexcept;
    TResult<void> Resize(u32 width, u32 height) noexcept;

    // モーションパス開始: motion RT を 0 クリア + 内部 depth を bind してパイプライン設定。
    //   view_proj      : 現フレームの jitter なし VP
    //   prev_view_proj : 前フレームの jitter なし VP
    // motion vector は jitter なしの VP で計算する (TAA jitter は color pass 専用)。
    void Begin(IRhiCommandList& cl,
               const FMat4& view_proj, const FMat4& prev_view_proj) noexcept;

    // 1 mesh の motion vector を描画。静的 mesh は prev_model に model と同値を渡す。
    void DrawMesh(IRhiCommandList& cl, const FGpuMesh& mesh,
                  const FMat4& model, const FMat4& prev_model) noexcept;

    // モーションパス終了 (main pass の RT へ復帰)。
    void End(IRhiCommandList& cl) noexcept;

    // 出力 motion vector テクスチャ (RG16F、.rg = prev_uv - curr_uv)。
    IRhiTexture* OutputTexture() const noexcept { return _motion.Get(); }

    // 出力 world-space normal テクスチャ (RGBA16F、.xyz = normalized world normal)。
    IRhiTexture* OutputNormalTexture() const noexcept { return _normal.Get(); }

private:
    TResult<void> CreateTargets(IRhiDevice& device, u32 w, u32 h) noexcept;
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice*             _device = nullptr;
    u32                     _width  = 0;
    u32                     _height = 0;
    FMat4                    _vp{};        // Begin で渡された現フレーム VP
    FMat4                    _prev_vp{};   // Begin で渡された前フレーム VP

    TUniquePtr<IRhiTexture>  _motion;      // RG16F、screen-space motion (prev_uv - curr_uv)
    TUniquePtr<IRhiTexture>  _normal;      // RGBA16F、world-space normal (.xyz)
    TUniquePtr<IRhiTexture>  _depth;       // D32、occlusion 用の内部 depth
    TUniquePtr<IRhiShader>   _vs;
    TUniquePtr<IRhiShader>   _ps;
    TUniquePtr<IRhiPipeline> _pipeline;
    TUniquePtr<IRhiBuffer>   _cb;          // MotionCB { curr_mvp, prev_mvp, curr_model }
};

} // namespace acs
