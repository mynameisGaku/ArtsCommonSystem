// SPDX-License-Identifier: Apache-2.0
// Screen-Space Reflection
//
// 設計:
//   - シーン描画後の HDR scene color + depth-SRV を入力にして、各 pixel から
//     reflection ray を screen space で march
//   - 衝突したら scene_color を sample、ray 起点に reflection を加算
//   - 結果は別 HDR RT (= ssr_rt) に書き出し → caller が composite で本 HDR へ加算
//   - normal は FMotionVector パスが出力する normal G-buffer (RGBA16F world normal)
//     から sample する。depth-derivative cross(ddx,ddy) は 2x2 quad
//     単位で faceted になり、曲面の反射ベクトルが段差状になってガビガビになる
//
// ray march は screen-space DDA (McGuire & Mara 2014)。反射レイを
// screen 空間へ射影し 1 texel/step で行進する。world 空間固定ステップ march は
// screen 空間でサンプリングが疎になり、反射先がカメラ近傍/grazing だと 1 step が
// 多 pixel に飛んで反射像がレイ方向に伸びてスメアする — DDA で根本解決。
//
// temporal accumulation: raw SSR を per-frame jitter 付きで撃ち、
// 履歴 (reproject + neighborhood clamp) と時間方向に平均することで、ray-march の
// silhouette ジャギーを均す。OutputTexture() は temporal 累積後を返す。
//
// glossy reflection (roughness 別 mip サンプル) は未対応 — roughness 依存の blend は
// FPbrShader 側が担当。
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
 * Screen-Space Reflection。HDR scene color + depth から反射を screen 空間で焼く。
 *
 * @details
 * シーン描画後の HDR scene color と depth-SRV を入力に、各 pixel から反射 ray を
 * screen-space DDA (McGuire & Mara 2014) で 1 texel/step march し、衝突先の
 * scene_color を集める。raw march (jitter 付き) → temporal accumulation の 2 pass
 * 構成で、履歴を reproject + neighborhood clamp して silhouette ジャギーを均す。
 * GPU リソースは TUniquePtr で単独所有する non-copy 型。
 */
class FSsr {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FSsr() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FSsr() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FSsr(const FSsr&) = delete;

    /** コピー代入も禁止。 */
    FSsr& operator=(const FSsr&) = delete;

    /**
     * GPU リソース (出力 RT・history ping-pong・パイプライン・CB) を確保する。
     *
     * @param device RT・パイプライン生成に使う RHI デバイス。
     * @param hdr_format SSR 出力テクスチャのフォーマット (シーン HDR と同じ R16G16B16A16F 推奨)。
     * @param width 出力 RT の幅。
     * @param height 出力 RT の高さ。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, EFormat hdr_format,
                       u32 width, u32 height) noexcept;

    /** 確保した GPU リソースを解放し、temporal 履歴をリセットする。 */
    void Shutdown() noexcept;

    /**
     * 解像度変更時に出力 RT と history を作り直す (ウィンドウリサイズで呼ぶ)。
     *
     * @details サイズが変わると history は再利用できないため temporal 累積をリセットする。
     * @param width 新しい出力 RT の幅。
     * @param height 新しい出力 RT の高さ。
     * @return 成功なら空の TResult、Init 前の呼び出しや再確保失敗ならエラー。
     */
    TResult<void> Resize(u32 width, u32 height) noexcept;

    /**
     * SSR を計算する (raw march → temporal accumulation の 2 pass)。
     *
     * @details
     * Pass1 で jitter 付き raw SSR を m_Output に焼き、Pass2 で前フレームの history と
     * reproject + neighborhood clamp して時間平均する。
     * @param device RHI デバイス (現状未使用、コマンドは cl 経由で積む)。
     * @param cl 描画コマンドを積むコマンドリスト。
     * @param scene_color 現フレームの HDR scene color。
     * @param scene_depth 現フレームの depth buffer (shader_visible_depth=true 必須)。
     * @param normal_gbuffer FMotionVector パスの world-space normal G-buffer (RGBA16F)。
     * @param view_proj 現フレームの view-projection 行列 (row-major)。
     * @param inv_view_proj view_proj の逆行列 (world pos 復元用)。
     * @param prev_view_proj 前フレームの view_proj (temporal reproject 用、identity で reprojection 無効)。
     * @param eye カメラのワールド位置。
     * @param intensity SSR 強度 (0..2)。
     * @param motion_texture 非 null なら temporal pass が動く mesh の反射を motion vector で reproject する (null なら camera-only depth reproject)。FMotionVector::OutputTexture() を渡す。
     * @param hiz_texture 非 null なら 1/8 解像度 coarse min-depth として ray march の skip-ahead に使う (FHiZ::Texture())。null なら 1 texel/step DDA。空中レイで 4-6 倍高速化。
     */
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_color, IRhiTexture& scene_depth,
                IRhiTexture& normal_gbuffer,
                const FMat4& view_proj, const FMat4& inv_view_proj,
                const FMat4& prev_view_proj,
                FVec3 eye, f32 intensity = 0.6f,
                IRhiTexture* motion_texture = nullptr,
                IRhiTexture* hiz_texture    = nullptr) noexcept;

    /**
     * temporal accumulation 後の history テクスチャを返す (FPbrShader / overlay 用)。
     *
     * @return 直近に書き込んだ history RT (frame 0 では history[0])。
     */
    IRhiTexture* OutputTexture() const noexcept {
        return m_History[m_TemporalFrame == 0u ? 0u : ((m_TemporalFrame - 1u) & 1u)].Get();
    }

    /**
     * temporal 前の raw SSR テクスチャ (jitter 付き march 結果) を返す。
     *
     * @return raw SSR RT。
     */
    IRhiTexture*  RawTexture()    const noexcept { return m_Output.Get(); }

    /**
     * 出力テクスチャのフォーマットを返す。
     *
     * @return Init で指定した HDR フォーマット。
     */
    EFormat        OutputFormat() const noexcept { return m_HdrFormat; }

private:
    /**
     * 出力 RT と temporal history ping-pong を生成する。
     *
     * @param device テクスチャ生成に使う RHI デバイス。
     * @param width 生成する RT の幅。
     * @param height 生成する RT の高さ。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept;

    /**
     * raw SSR と temporal の VS/PS/PSO を生成する。
     *
     * @param device パイプライン生成に使う RHI デバイス。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    /** Init で受け取った device (Resize で再利用)。 */
    IRhiDevice*             m_Device = nullptr;

    /** 出力 RT の幅。 */
    u32                     m_Width  = 0;

    /** 出力 RT の高さ。 */
    u32                     m_Height = 0;

    /** 出力テクスチャのフォーマット (既定 R16G16B16A16_Float)。 */
    EFormat                  m_HdrFormat = EFormat::R16G16B16A16_Float;

    /** raw SSR の出力 RT (jitter 付き march 結果)。 */
    TUniquePtr<IRhiTexture>  m_Output;

    /** temporal accumulation の history ping-pong (2 枚)。 */
    TUniquePtr<IRhiTexture>  m_History[2];

    /** フルスクリーン三角形の頂点シェーダ (raw/temporal 共用)。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** raw SSR の ray march ピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** temporal accumulation のピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_TemporalPs;

    /** raw SSR 描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** temporal accumulation 描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_TemporalPipeline;

    /** 定数バッファ (カメラ行列・パラメータを渡す)。 */
    TUniquePtr<IRhiBuffer>   m_Cb;

    /** temporal フレームカウンタ (history ping-pong と jitter に使う)。 */
    u32                     m_TemporalFrame = 0;
};

} // namespace acs
