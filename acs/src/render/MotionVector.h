// SPDX-License-Identifier: Apache-2.0
// motion + normal G-buffer の geometry pass
//
// シーンの全 mesh を再ラスタライズし、MRT 2 枚を書き出す:
//   - motion (RG16F)  : screen-space motion vector (prev_uv - curr_uv)。camera
//                       動きと object 動きの両方を含む (前フレームの model + VP)。
//                       TAA が history を正確に reproject し ghost / trail を消す。
//   - normal (RGBA16F): world-space normal。頂点法線をピクセル補間したもので、
//                       depth-derivative の cross(ddx,ddy) と違い曲面でも段差が
//                       出ない。SSR/SSGI/SSAO がこれを sample し、faceted な
//                       反射ベクトル由来のジャギーを根本解消する。
//
// depth からの camera reprojection のみでは動く mesh が ghost するため、
// 本モジュールがその穴を埋める。
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
//   if (mv.Begin(*cl, vp_no_jitter, prev_vp_no_jitter)) {
//       for (each mesh) mv.DrawMesh(*cl, gm, curr_model, prev_model);
//       mv.End(*cl);
//       post_params.taa_motion_texture = mv.OutputTexture();
//   }
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"
#include "render/RenderAssets.h"        // FGpuMesh
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"

namespace acs {

/**
 * motion vector + world-space normal の G-buffer geometry pass。
 *
 * @details
 * シーンの全 mesh を再ラスタライズし、MRT 2 枚 (motion RG16F + normal RGBA16F) を
 * 書き出す。motion は screen-space motion vector (prev_uv - curr_uv) で camera 動きと
 * object 動きの両方を含み、TAA が history を正確に reproject して ghost/trail を消す。
 * normal は頂点法線をピクセル補間した world-space 法線で、SSR/SSGI/SSAO が sample する。
 * FShadowMap と同じ Begin/DrawMesh/End パターンで、occlusion 用 depth を内部に持つ。
 */
class FMotionVector {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FMotionVector() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FMotionVector() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FMotionVector(const FMotionVector&)            = delete;

    /** コピー代入も禁止。 */
    FMotionVector& operator=(const FMotionVector&) = delete;

    /**
     * GPU リソース (RT 2 枚 + depth + パイプライン) を確保する。
     *
     * @param device RT・パイプライン生成に使う RHI デバイス。
     * @param width 出力 G-buffer の幅。
     * @param height 出力 G-buffer の高さ。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;

    /** 確保した GPU リソースを解放する (多重呼び出し安全)。 */
    void Shutdown() noexcept;

    /**
     * 解像度変更時に内部 RT を作り直す (ウィンドウリサイズで呼ぶ)。
     *
     * @param width 新しい出力 G-buffer の幅。
     * @param height 新しい出力 G-buffer の高さ。
     * @return 成功なら空の TResult、再確保失敗ならエラー。
     */
    TResult<void> Resize(u32 width, u32 height) noexcept;

    /**
     * モーションパスを開始する (motion RT を 0 クリア + 内部 depth を bind してパイプライン設定)。
     *
     * @details motion vector は jitter なしの VP で計算する (TAA jitter は color pass 専用)。
     * @param cl コマンドを積むコマンドリスト。
     * @param view_proj 現フレームの jitter なし VP。
     * @param prev_view_proj 前フレームの jitter なし VP。
     * @return true only when both MRT attachments and depth were bound. A
     *         false result leaves the pass inactive; DrawMesh() and End()
     *         become no-ops until a later successful Begin().
     */
    bool Begin(IRhiCommandList& cl,
               const FMat4& view_proj, const FMat4& prev_view_proj) noexcept;

    /**
     * 1 mesh の motion vector を描画する。
     *
     * @details 静的 mesh は prev_model に model と同値を渡す。
     * @param cl コマンドを積むコマンドリスト。
     * @param mesh 描画する GPU mesh。
     * @param model 現フレームの model 行列。
     * @param prev_model 前フレームの model 行列。
     */
    void DrawMesh(IRhiCommandList& cl, const FGpuMesh& mesh,
                  const FMat4& model, const FMat4& prev_model) noexcept;

    /**
     * モーションパスを終了する (main pass の RT へ復帰)。
     *
     * @param cl コマンドを積むコマンドリスト。
     */
    void End(IRhiCommandList& cl) noexcept;

    /**
     * 出力 motion vector テクスチャを返す。
     *
     * @return motion RT (RG16F、.rg = prev_uv - curr_uv)。
     */
    IRhiTexture* OutputTexture() const noexcept { return m_Motion.Get(); }

    /**
     * 出力 world-space normal テクスチャを返す。
     *
     * @return normal RT (RGBA16F、.xyz = normalized world normal)。
     */
    IRhiTexture* OutputNormalTexture() const noexcept { return m_Normal.Get(); }

private:
    /**
     * 出力 RT (motion + normal) と内部 depth を生成する。
     *
     * @param device RT 生成に使う RHI デバイス。
     * @param w 出力幅。
     * @param h 出力高さ。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreateTargets(IRhiDevice& device, u32 w, u32 h) noexcept;

    /**
     * G-buffer 描画用の VS/PS/PSO を生成する。
     *
     * @param device パイプライン生成に使う RHI デバイス。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    /** Init で受け取った device (Resize で再利用)。 */
    IRhiDevice*             m_Device = nullptr;

    /** 出力 G-buffer の幅。 */
    u32                     m_Width  = 0;

    /** 出力 G-buffer の高さ。 */
    u32                     m_Height = 0;

    /** Begin で渡された現フレーム VP。 */
    FMat4                    m_Vp{};

    /** Begin で渡された前フレーム VP。 */
    FMat4                    m_PrevVp{};

    /** 出力 motion RT (RG16F、screen-space motion = prev_uv - curr_uv)。 */
    TUniquePtr<IRhiTexture>  m_Motion;

    /** 出力 normal RT (RGBA16F、world-space normal の .xyz)。 */
    TUniquePtr<IRhiTexture>  m_Normal;

    /** occlusion 用の内部 depth (D32、scene depth とは共有しない)。 */
    TUniquePtr<IRhiTexture>  m_Depth;

    /** G-buffer 描画の頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** motion vector と world normal を書き出すピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** G-buffer 描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /**
     * Per-draw constant-buffer ring.
     *
     * A single mapped upload CB cannot be overwritten between draws in one
     * command list: raw DX12 consumes it later on the GPU and every draw would
     * otherwise observe the final object's matrices.
     */
    static constexpr u32     kObjectCbRing = 256;
    TUniquePtr<IRhiBuffer>   m_Cbs[kObjectCbRing];
    u32                      m_DrawCursor = 0;

    /**
     * True only between a successful MRT Begin and its matching End.
     *
     * The RHI can reject an attachment set without changing backend state.
     * Keeping that result here prevents stale-target draws and prevents End
     * from unbinding or transitioning resources that were never bound.
     */
    bool                     m_PassActive = false;
};

} // namespace acs
