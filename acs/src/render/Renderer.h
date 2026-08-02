// SPDX-License-Identifier: Apache-2.0
// 高レベル CRenderer（ウィンドウへの描画ループを統括する司令塔）
//
// 使い方 (典型的):
//   CRenderer rdr;
//   rdr.Init(window);
//   while (!window.ShouldClose()) {
//       window.PollEvents();
//       rdr.BeginFrame({0.1f, 0.2f, 0.3f, 1.0f});
//       // BeginFrame 後は GetCommandList() でコマンドを積める。
//       // 高レベルヘルパ:
//       //   CStandardShader  — Lambert+Blinn-Phong + シャドウマップ
//       //   CSkinnedShader   — GPU スキニング (BoneCB + BLENDINDICES/WEIGHT)
//       //   CSky             — 手続き生成スカイ (Day/Sunset/Night プリセット)
//       //   CSpriteBatch     — 2D スプライト + フォント
//       //   FFont            — TTrueType -> アトラス -> CSpriteBatch
//       //   CParticleSystem  — 簡易 GPU パーティクル
//       //   CShadowMap       — depth-only パス
//       //   CPostProcess     — HDR + Bloom + ACES Tonemap (Diligent backend 専用)
//       if (!rdr.EndFrame()) break;
//   }
//   rdr.Shutdown();
//
// HDR ポストプロセス経路を組むときは CApplication::OnCustomFrame() を override
// して、自分で BeginRenderToTexture(HDR_RT) → 描画 → CPostProcess.Render() →
// Present までを直接書く (HelloBloom 参照)。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"

namespace acs {

class FWindow;

/**
 * ウィンドウへの描画ループを統括する高レベルレンダラ。
 *
 * @details
 * Device + Swapchain + CommandList (+ 任意で深度バッファ) を所有し、BeginFrame /
 * EndFrame でフレーム境界を管理する。BeginFrame 後に GetCommandList() でコマンドを
 * 積み、CStandardShader / CSky / CSpriteBatch などの高レベルヘルパで描画する。GPU
 * リソースを単独所有する non-copy 型。
 */
class CRenderer {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CRenderer() noexcept = default;

    /** 破棄する (確保した GPU リソースを解放)。 */
    ~CRenderer() noexcept;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CRenderer(const CRenderer&) = delete;

    /** コピー代入も禁止。 */
    CRenderer& operator=(const CRenderer&) = delete;

    /**
     * ウィンドウに紐付けて初期化する (Device + Swapchain + CommandList を作成)。
     *
     * @param w 描画先のウィンドウ。
     * @param enable_debug デバッグレイヤを有効にするか。
     * @param enable_depth true なら深度バッファ (D32_Float) を自動で作成する。
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    TResult<void> Init(FWindow& w, bool enable_debug = false, bool enable_depth = true) noexcept;

    /**
     * 外部 HWND に紐付けて初期化する (FWindow を経由せず生 HWND をホストする)。
     *
     * @details
     * C# WPF エディタ等が用意した既存ウィンドウの HWND にスワップチェインを作る。
     * FWindow 版 Init と同様に Device + Swapchain + CommandList (+深度) を確保する。
     * リサイズは OnResize(w,h) を呼ぶ (HWND からサイズは取得しない)。
     * @param hwnd 描画先の生 HWND (void*)。
     * @param width 初期バックバッファ幅。
     * @param height 初期バックバッファ高さ。
     * @param enable_debug デバッグレイヤを有効にするか。
     * @param enable_depth true なら深度バッファを自動作成する。
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    TResult<void> InitExternal(void* hwnd, u32 width, u32 height,
                               bool enable_debug = false, bool enable_depth = true) noexcept;

    /** 確保した全リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * フレームを開始する (クリア色で塗りつぶし、深度は 1.0 でクリア)。
     *
     * @param clear バックバッファをクリアする色。
     */
    void BeginFrame(const FClearColor& clear) noexcept;

    /**
     * Return false instead of waiting when the backend's current frame slot is
     * still owned by the GPU. Recording and RHI ownership stay on the caller's
     * existing render thread.
     */
    bool TryBeginFrameWithoutGpuWait(const FClearColor& clear) noexcept;

    /** Query the same frame-slot gate without mutating renderer state. */
    bool CanBeginFrameWithoutGpuWait() const noexcept;

    /** Return false once the backend reports device removal/loss. */
    bool IsOperational() const noexcept;

    /**
     * フレームを終了する (コマンドを GPU に投入し Present)。
     *
     * @return Submit と Present の両方が成功したとき true。
     */
    bool EndFrame() noexcept;

    /**
     * Submit and present without waiting for the following frame slot. The
     * next TryBeginFrameWithoutGpuWait call reports backpressure instead.
     */
    bool EndFrameWithoutGpuWait() noexcept;

    /**
     * ウィンドウサイズ変更時に呼ぶ (スワップチェーン・深度を再作成)。
     *
     * @param width 新しいウィンドウ幅。
     * @param height 新しいウィンドウ高さ。
     */
    bool OnResize(u32 width, u32 height) noexcept;

    /**
     * RHI デバイスを返す。
     *
     * @return 所有する RHI デバイス。
     */
    IRhiDevice*     Device()      const noexcept { return m_Device.Get(); }

    /**
     * スワップチェーンを返す。
     *
     * @return 所有するスワップチェーン。
     */
    IRhiSwapchain*  Swapchain()   const noexcept { return m_Swapchain.Get(); }

    /**
     * コマンドリストを返す (BeginFrame 後にコマンドを積む)。
     *
     * @return 所有するコマンドリスト。
     */
    IRhiCommandList* CommandList() const noexcept { return m_Cmd.Get(); }

    /**
     * 深度バッファを返す。
     *
     * @return 所有する深度バッファ (enable_depth=false なら nullptr)。
     */
    IRhiTexture*    DepthBuffer() const noexcept { return m_Depth.Get(); }

    /**
     * BeginFrame で取得した現在のバックバッファ index を返す。
     *
     * @details マルチパス (反射等) で同一フレーム内にスワップチェーンを再バインドする際に必要。
     * @return 現在のバックバッファ index。
     */
    u32             CurrentBuffer() const noexcept { return m_CurrentBuffer; }

    /**
     * カラー描画ターゲットのフォーマットを返す (パイプライン作成時に必要)。
     *
     * @return バックバッファのカラーフォーマット。
     */
    EFormat          ColorFormat() const noexcept { return m_ColorFormat; }

    /**
     * 深度ターゲットのフォーマットを返す (パイプライン作成時に必要)。
     *
     * @return 深度バッファのフォーマット。
     */
    EFormat          DepthFormat() const noexcept { return m_DepthFormat; }

private:
    bool BeginFrameInternal(
        const FClearColor& clear, bool avoid_gpu_wait) noexcept;
    bool EndFrameInternal(bool avoid_gpu_wait) noexcept;
    /**
     * 深度バッファを指定サイズで作り直す。
     *
     * @param w 新しい深度バッファの幅。
     * @param h 新しい深度バッファの高さ。
     * @return 成功なら空の TResult、再作成失敗ならエラー。
     */
    TResult<void> RebuildDepth(u32 w, u32 h) noexcept;

    /** RHI デバイス。 */
    TUniquePtr<IRhiDevice>      m_Device;

    /** スワップチェーン。 */
    TUniquePtr<IRhiSwapchain>   m_Swapchain;

    /** コマンドリスト。 */
    TUniquePtr<IRhiCommandList> m_Cmd;

    /** 深度バッファ (enable_depth=false なら未確保)。 */
    TUniquePtr<IRhiTexture>     m_Depth;

    /** バックバッファのカラーフォーマット。 */
    EFormat                      m_ColorFormat  = EFormat::B8G8R8A8_UNorm;

    /** 深度バッファのフォーマット。 */
    EFormat                      m_DepthFormat  = EFormat::D32_Float;

    /** BeginFrame で取得した現在のバックバッファ index。 */
    u32                         m_CurrentBuffer = 0;

    /** 深度バッファを使うか。 */
    bool                        m_EnableDepth   = true;

    /** フレームが BeginFrame 済み (EndFrame 未呼出) か。 */
    bool                        m_bFrameOpen     = false;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FRenderer = CRenderer;


} // namespace acs
