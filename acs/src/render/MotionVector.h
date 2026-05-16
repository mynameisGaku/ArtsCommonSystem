// 動的 mesh motion vector G-buffer (Phase 34f-3)
//
// シーンの全 mesh を再ラスタライズし、各 pixel の screen-space motion vector
// (prev_uv - curr_uv) を RG16F テクスチャへ書き出す。motion vector は camera 動き
// と object 動きの両方を含む (前フレームの model 行列 + 前フレームの VP で
// prev clip pos を求める)。TAA はこのテクスチャで history を正確に reproject し、
// 動く mesh の ghost / trail を消す。
//
// Phase 34f-2 までの TAA は depth からの camera reprojection のみ対応で、
// 動く mesh は ghost していた。本モジュールがその穴を埋める。
//
// 設計:
//   - ShadowMap と同じ Begin/DrawMesh/End パターン (caller がシーンを描く)
//   - 全 mesh を描く前提 (静的 mesh は prev_model == model)。motion texture は
//     画面全体で authoritative になり、TAA は depth を併用せず済む
//     (→ TAA resolve PSO の texture slot を増やさず slot binding 問題を回避)
//   - occlusion 用に専用 depth buffer を内部に持つ (scene depth は共有しない)
//
// 使い方:
//   MotionVector mv;
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

class MotionVector {
public:
    MotionVector() noexcept = default;
    ~MotionVector() noexcept = default;

    MotionVector(const MotionVector&)            = delete;
    MotionVector& operator=(const MotionVector&) = delete;

    Result<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;
    void Shutdown() noexcept;
    Result<void> Resize(u32 width, u32 height) noexcept;

    // モーションパス開始: motion RT を 0 クリア + 内部 depth を bind してパイプライン設定。
    //   view_proj      : 現フレームの jitter なし VP
    //   prev_view_proj : 前フレームの jitter なし VP
    // motion vector は jitter なしの VP で計算する (TAA jitter は color pass 専用)。
    void Begin(IRhiCommandList& cl,
               const Mat4& view_proj, const Mat4& prev_view_proj) noexcept;

    // 1 mesh の motion vector を描画。静的 mesh は prev_model に model と同値を渡す。
    void DrawMesh(IRhiCommandList& cl, const GpuMesh& mesh,
                  const Mat4& model, const Mat4& prev_model) noexcept;

    // モーションパス終了 (main pass の RT へ復帰)。
    void End(IRhiCommandList& cl) noexcept;

    // 出力 motion vector テクスチャ (RG16F、.rg = prev_uv - curr_uv)。
    IRhiTexture* OutputTexture() const noexcept { return _motion.Get(); }

private:
    Result<void> CreateTargets(IRhiDevice& device, u32 w, u32 h) noexcept;
    Result<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice*             _device = nullptr;
    u32                     _width  = 0;
    u32                     _height = 0;
    Mat4                    _vp{};        // Begin で渡された現フレーム VP
    Mat4                    _prev_vp{};   // Begin で渡された前フレーム VP

    UniquePtr<IRhiTexture>  _motion;      // RG16F、screen-space motion (prev_uv - curr_uv)
    UniquePtr<IRhiTexture>  _depth;       // D32、occlusion 用の内部 depth
    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiPipeline> _pipeline;
    UniquePtr<IRhiBuffer>   _cb;          // MotionCB { curr_mvp, prev_mvp }
};

} // namespace acs
