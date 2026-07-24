// SPDX-License-Identifier: Apache-2.0
// Screen-Space Ambient Occlusion
//
// HBAO-lite: depth-only horizon-based AO。各 pixel から N 方向に screen-space で
// march して horizon angle を求め、ambient occlusion を出す。
//
// 出力は R8G8B8A8_UNorm の RT。.r = AO visibility、.g = contact shadow
// (ともに 1=照明 / 0=遮蔽)。FPbrShader が .r を ambient、.g を direct light に乗算する。
//
// 制限:
//   - 法線は FMotionVector の normal G-buffer (world normal) を view 空間へ変換して
//     使う (depth 微分 cross(ddx,ddy) は faceted で AO がブロック状になる)
//   - 単純な uniform random direction、Halton 等の low-discrepancy 未使用
//   - 6 direction × 6 step = 36 sample / pixel
//   - depth-aware bilateral blur pass で raw → blurred を生成。
//     OutputTexture() は blur 後を返す。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"

namespace acs {

/**
 * depth ベースの Screen-Space Ambient Occlusion (GTAO + contact shadow)。
 *
 * @details
 * 各 pixel で view ベクトルに対する両側の horizon angle を screen-march で求め、
 * 法線を slice 平面に射影した角度を使って cosine-weighted visibility を analytical に
 * 積分する (GTAO)。同時に dir light 0 方向へ短距離 ray march して接地影 (contact shadow)
 * を求める。出力は R8G8B8A8_UNorm で .r=AO visibility、.g=contact shadow (ともに 1=照明 /
 * 0=遮蔽)。raw → depth-aware bilateral blur の 2 pass で生成し、OutputTexture() は blur 後を返す。
 */
class FSsao {
public:
    /** Backend-compiled shader handles awaiting owner-thread PSO creation. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> vertex;
        TUniquePtr<IRhiShader> pixel;
        TUniquePtr<IRhiShader> blur_pixel;

        /** Aggregate a backend-managed asynchronous compile without waiting. */
        EShaderStatus Status() const noexcept;
    };

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FSsao() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FSsao() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FSsao(const FSsao&) = delete;

    /** コピー代入も禁止。 */
    FSsao& operator=(const FSsao&) = delete;

    /**
     * 出力 RT・パイプライン・定数バッファを生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param width 出力解像度の幅。
     * @param height 出力解像度の高さ。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;

    /**
     * Submit all SSAO shader stages to a supporting asynchronous backend.
     *
     * @details The returned handles must be polled with FCompiledShaders::Status
     * and committed with InitWithCompiledShaders on the render-owner thread.
     */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(
        IRhiDevice& device) noexcept;

    /**
     * Create render targets, PSOs and the constant buffer from ready shaders.
     *
     * @param device Render-owner RHI device.
     * @param shaders Ready vertex, GTAO pixel and blur pixel shaders.
     * @param width Output width.
     * @param height Output height.
     */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        u32 width,
        u32 height) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * 解像度変更時に内部 RT を作り直す (ウィンドウリサイズで呼ぶ)。
     *
     * @param width 新しい出力幅。
     * @param height 新しい出力高さ。
     * @return 成功なら空の TResult、Init 前の呼び出しや再確保失敗ならエラー。
     */
    TResult<void> Resize(u32 width, u32 height) noexcept;

    /**
     * SSAO (GTAO) と contact shadow を計算して内部 RT に書く。
     *
     * @details raw pass で m_Output に焼き、depth-aware bilateral blur pass で m_BlurOutput に平滑化する。
     * @param device 描画に使う RHI デバイス (現状未使用)。
     * @param cl コマンドを積むコマンドリスト。
     * @param scene_depth shader_visible_depth=true な depth buffer。
     * @param normal_gbuffer FMotionVector の world-space normal G-buffer (RGBA16F)。
     * @param view_proj 現フレームの view * projection (contact shadow の投影に使う)。
     * @param inv_view_proj 現フレーム VP の逆 (depth+uv → world)。
     * @param view world → view 変換 (GTAO の slice 計算は view 空間で行う)。
     * @param eye カメラの world pos。
     * @param light_dir dir light 0 への方向 (world、surface→light)。contact shadow の march 方向。
     * @param intensity 遮蔽強度 (0=AO 無効、1=neutral、>1=過度に暗く)。
     * @param radius AO の最大半径 (world units、0.5 ~ 2.0 が典型)。
     */
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_depth,
                IRhiTexture& normal_gbuffer,
                const FMat4& view_proj,
                const FMat4& inv_view_proj,
                const FMat4& view,
                FVec3 eye, FVec3 light_dir,
                f32 intensity = 1.0f,
                f32 radius    = 0.5f) noexcept;

    /**
     * blur 後の RT を返す (FPbrShader / overlay はこちらを読む)。
     *
     * @return depth-aware bilateral blur 後の AO/contact RT。
     */
    IRhiTexture* OutputTexture() const noexcept { return m_BlurOutput.Get(); }

    /**
     * raw (blur 前) の RT を返す (デバッグ用途向け)。
     *
     * @return blur 前の AO/contact raw RT。
     */
    IRhiTexture* RawTexture() const noexcept { return m_Output.Get(); }

private:
    /**
     * 出力 RT (raw と blur 後) を生成する。
     *
     * @param device RT 生成に使う RHI デバイス。
     * @param w 出力幅。
     * @param h 出力高さ。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreateOutputRT(IRhiDevice& device, u32 w, u32 h) noexcept;

    /**
     * SSAO 本体と blur のシェーダ・パイプラインを生成する。
     *
     * @param device パイプライン生成に使う RHI デバイス。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreatePipeline(
        IRhiDevice& device,
        FCompiledShaders& shaders) noexcept;

    /** Init で受け取った device (Resize で再利用)。 */
    IRhiDevice*             m_Device = nullptr;

    /** 出力 RT の幅。 */
    u32                     m_Width  = 0;

    /** 出力 RT の高さ。 */
    u32                     m_Height = 0;

    /** SSAO raw 出力 RT (.r=AO、.g=contact shadow)。 */
    TUniquePtr<IRhiTexture>  m_Output;

    /** depth-aware bilateral blur 後の出力 RT。 */
    TUniquePtr<IRhiTexture>  m_BlurOutput;

    /** フルスクリーン三角形の頂点シェーダ (本体・blur 共用)。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** GTAO + contact shadow を計算するピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** depth-aware bilateral blur のピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_BlurPs;

    /** SSAO 本体の描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** blur の描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_BlurPipeline;

    /** SSAO パラメータの定数バッファ。 */
    TUniquePtr<IRhiBuffer>   m_Cb;
};

} // namespace acs
