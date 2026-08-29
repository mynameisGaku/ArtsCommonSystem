// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_RENDERER_H
#define ACS_RENDER_RENDERER_H

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/RendererFrameEndResult.h"

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
    /** フレーム開始を受け取る非所有通知関数。 */
    using FFrameBeginListener = void (*)(void* listener) noexcept;

    /** フレーム終了結果を受け取る非所有通知関数。 */
    using FFrameEndListener = void (*)(
        void* listener,
        const FRendererFrameEndResult& result) noexcept;

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CRenderer() noexcept = default;

    /** 破棄する (確保した GPU リソースを解放)。 */
    ~CRenderer() noexcept;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CRenderer(const CRenderer&) = delete;

    /** コピー代入も禁止。 */
    CRenderer& operator=(const CRenderer&) = delete;

    /**
     * BeginFrame成功とEndFrame結果を同じ非所有通知先へ結び付ける。
     * null、または別の通知先が登録済みならfalseを返す。
     */
    bool TryBindFrameLifecycleListener(
        void* listener,
        FFrameBeginListener begin_listener,
        FFrameEndListener end_listener) noexcept;

    /** 指定した通知先が登録中の場合だけフレーム通知を解除する。 */
    void UnbindFrameLifecycleListener(void* listener) noexcept;

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

    /** GPUが現在の枠を使用中なら待機せずfalseを返し、呼び出し元の描画スレッドを保つ。 */
    bool TryBeginFrameWithoutGpuWait(const FClearColor& clear) noexcept;

    /** 状態を変えず、待機なしで次のフレームを開始できるか返す。 */
    bool CanBeginFrameWithoutGpuWait() const noexcept;

    /** 描画装置の切断または消失が報告された後はfalseを返す。 */
    bool IsOperational() const noexcept;

    /** BeginFrame後かつEndFrame前ならtrueを返す。状態は変更しない。 */
    bool IsFrameOpen() const noexcept { return m_bFrameOpen; }

    /** 開いているフレームの提出IDを返す。フレーム外では0。 */
    u64 CurrentFrameSubmissionId() const noexcept
    {
        return m_CurrentFrameSubmissionId;
    }

    /**
     * 開いているフレームがなければ、提出済みGPU処理の完了を待つ。
     *
     * @return 待機できた場合はtrue。未提出フレームが開いている場合はfalse。
     */
    bool TryWaitForGpuIdle() noexcept;

    /**
     * フレームを終了する (コマンドを GPU に投入し Present)。
     *
     * @return Submit と Present の両方が成功したとき true。
     */
    bool EndFrame() noexcept;

    /**
     * GPU提出と画面提示の成否を分けてフレームを終了する。
     *
     * @return SubmitとPresentを個別に保持した結果。
     */
    FRendererFrameEndResult EndFrameDetailed() noexcept;

    /** 次の枠を待たずに提出・表示し、混雑は次回の待機なし開始で返す。 */
    bool EndFrameWithoutGpuWait() noexcept;

    /**
     * 次のフレーム枠を待たず、GPU提出と画面提示の成否を分けて終了する。
     *
     * @return SubmitとPresentを個別に保持した結果。
     */
    FRendererFrameEndResult EndFrameWithoutGpuWaitDetailed() noexcept;

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
    /** 登録中の通知先へフレーム開始を一度通知する。 */
    void NotifyFrameBegin_Internal() noexcept;

    /** 登録中の通知先へフレーム終了結果を一度通知する。 */
    void NotifyFrameEnd_Internal(
        const FRendererFrameEndResult& result) noexcept;

    bool BeginFrameInternal(
        const FClearColor& clear, bool avoid_gpu_wait) noexcept;
    FRendererFrameEndResult EndFrameInternal(bool avoid_gpu_wait) noexcept;
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

    /** 現在記録中のフレームに割り当てた提出ID。フレーム外では0。 */
    u64                         m_CurrentFrameSubmissionId = 0u;

    /** 次に開始できたフレームへ割り当てる提出ID。0は使い切りを表す。 */
    u64                         m_NextFrameSubmissionId = 1u;

    /** フレーム境界を受け取る非所有通知先。 */
    void*                       m_FrameListener = nullptr;

    /** フレーム開始を通知先へ渡す関数。 */
    FFrameBeginListener         m_FrameBeginListener = nullptr;

    /** フレーム終了結果を通知先へ渡す関数。 */
    FFrameEndListener           m_FrameEndListener = nullptr;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FRenderer = CRenderer;


} // namespace acs

#endif // ACS_RENDER_RENDERER_H
