// SPDX-License-Identifier: Apache-2.0
// Screen-Space Global Illumination
//
// 各ピクセルから法線半球内に N 本のレイを screen-space で march し、ヒットした
// pixel の HDR 色を 1 バウンス indirect light として集める。SSAO の構造に色
// サンプリングを追加した発展版 (Aaltonen 2014 系の単純化版)。
//
// 入力: scene_color (HDR R16G16B16A16_Float)、scene_depth (shader-visible depth)
// 出力: ssgi_color (RGB)、FPbrShader が ambient/indirect 項に加算
//
// 制限:
//   - 法線は FMotionVector の normal G-buffer (world normal) から sample
//     (depth 微分 cross(ddx,ddy) は faceted で hemisphere ray がブロック状になるため避ける)
//   - 8 step / 4 ray = 32 sample/pixel (画質と速度のバランス)
//   - 反射的なシャープなパスは捨て、diffuse-ish な広い hemisphere に絞る
//   - 1 bounce のみ (Lumen の voxel cone tracing 等は未対応)
//   - blur 無し (FPbrShader 側で linear sampling で smooth に補間する想定)
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
 * Screen-Space Global Illumination (1 バウンス indirect light)。
 *
 * @details
 * 各ピクセルから法線半球内に N 本のレイを screen-space で march し、ヒットした pixel の
 * HDR 色を 1 バウンスの indirect light として集める。SSAO の構造に色サンプリングを
 * 追加した発展版で、raw → depth-aware blur → temporal accumulation の 3 pass で構成する。
 * FPbrShader が結果を ambient/indirect 項に加算する。8 step / 4 ray = 32 sample/pixel、
 * diffuse-ish な広い hemisphere に絞り、反射的なシャープなパスは捨てる。
 */
class FSsgi {
public:
    /** CPU-compiled shader bytecode handed to the render-owner thread. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> vertex;
        TUniquePtr<IRhiShader> main_pixel;
        TUniquePtr<IRhiShader> blur_pixel;
        TUniquePtr<IRhiShader> temporal_pixel;
    };

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FSsgi() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FSsgi() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FSsgi(const FSsgi&) = delete;

    /** コピー代入も禁止。 */
    FSsgi& operator=(const FSsgi&) = delete;

    /**
     * 出力 RT・history・パイプライン・定数バッファを生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param width 出力解像度の幅。
     * @param height 出力解像度の高さ。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;

    /**
     * Compile all raw-DX12 SSGI shaders without touching an RHI device.
     * This is safe to run on a startup worker. Other backends return an
     * unsupported error and retain the regular owner-thread Init path.
     */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /**
     * Install CPU-compiled shaders and create the render targets, constant
     * buffer and PSOs. Must be called by the render-owner thread.
     */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        u32 width,
        u32 height) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * Keep allocated targets but make the next render a cold temporal start.
     * Call this when a shared render surface changes logical camera owner.
     */
    void InvalidateHistory() noexcept {
        m_TemporalFrame = 0u;
        m_OutputValid = false;
    }

    /**
     * 解像度変更時に内部 RT を作り直す (ウィンドウリサイズで呼ぶ)。
     *
     * @param width 新しい出力幅。
     * @param height 新しい出力高さ。
     * @return 成功なら空の TResult、再確保失敗ならエラー。
     */
    TResult<void> Resize(u32 width, u32 height) noexcept;

    /**
     * SSGI を計算して内部 RT に書く (raw → blur → temporal の 3 pass)。
     *
     * @param device 描画に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @param scene_color 現在フレームの HDR scene RT。
     * @param scene_depth shader-visible depth (SSR/SSAO と同じ)。
     * @param normal_gbuffer FMotionVector の world-space normal G-buffer (RGBA16F)。
     * @param view_proj 現フレームの view * projection。
     * @param inv_view_proj 現フレーム VP の逆 (depth+uv → world)。
     * @param prev_view_proj 前フレームの view_proj (temporal reproject 用)。identity を渡すと reprojection 無効 (= 静的 accumulate)。
     * @param eye カメラの world pos。
     * @param intensity indirect light の倍率 (0=無効、1=neutral、>1=強調)。
     * @param max_distance ray march の世界距離上限 (世界座標、典型 5.0)。
     * @param motion_texture 非 null なら temporal pass が depth reprojection ではなくこの motion vector で history を引く (動く mesh も ghost せず追従)。FMotionVector::OutputTexture() を渡す。null なら従来の camera-only depth reprojection。
     */
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_color,
                IRhiTexture& scene_depth,
                IRhiTexture& normal_gbuffer,
                const FMat4& view_proj,
                const FMat4& inv_view_proj,
                const FMat4& prev_view_proj,
                FVec3 eye,
                f32 intensity   = 1.0f,
                f32 max_distance = 5.0f,
                IRhiTexture* motion_texture = nullptr) noexcept;

    /**
     * temporal accumulation 後の history RT を返す。
     *
     * @details 直近の Render が書き込んだ index を返す。FPbrShader はこれを読む (blur + 時間積分でノイズ除去済)。
     * @return 直近の積分結果を持つ history RT。
     */
    IRhiTexture* OutputTexture() const noexcept {
        return m_History[m_TemporalFrame == 0u ? 0u : ((m_TemporalFrame - 1u) & 1u)].Get();
    }

    /** True only after raw, bilateral and temporal output were all recorded. */
    bool HasValidOutput() const noexcept { return m_OutputValid; }

    /**
     * raw (blur/temporal 前) の RT を返す (デバッグ用途向け)。
     *
     * @return SSGI raw RT。
     */
    IRhiTexture* RawTexture()    const noexcept { return m_Output.Get(); }

private:
    /**
     * 出力 RT (raw・blur 後・history ping-pong) を生成する。
     *
     * @param device RT 生成に使う RHI デバイス。
     * @param w 出力幅。
     * @param h 出力高さ。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreateOutputRT(IRhiDevice& device, u32 w, u32 h) noexcept;

    /**
     * Install 済みの SSGI 本体・blur・temporal シェーダから
     * 各描画パイプラインを生成する。
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

    /** SSGI raw 出力 RT。 */
    TUniquePtr<IRhiTexture>  m_Output;

    /** depth-aware bilateral blur 後の出力 RT。 */
    TUniquePtr<IRhiTexture>  m_BlurOutput;

    /** temporal accumulation 用の ping-pong history RT。 */
    TUniquePtr<IRhiTexture>  m_History[2];

    /** フルスクリーン三角形の頂点シェーダ (全 pass 共用)。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** hemisphere ray march で indirect light を集めるピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** depth-aware bilateral blur のピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_BlurPs;

    /** temporal accumulation のピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_TemporalPs;

    /** SSGI 本体の描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** blur の描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_BlurPipeline;

    /** temporal accumulation の描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_TemporalPipeline;

    /** SSGI パラメータの定数バッファ。 */
    TUniquePtr<IRhiBuffer>   m_Cb;

    /** temporal accumulation のフレームカウンタ (history の ping-pong に使う)。 */
    u32                     m_TemporalFrame = 0;

    /** Prevents callers from publishing an unwritten or invalidated history. */
    bool                    m_OutputValid = false;
};

} // namespace acs
