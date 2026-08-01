// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"
#include "render/RenderGraphAliasPlanSummary.h"

namespace acs {

class IRhiTexture;

/**
 * ポストプロセス効果のパラメータ一式。
 *
 * @details
 * Bloom / Tonemap / Cinematic FX / Color grading / CAS / TAA / Auto-exposure の
 * 全パラメータを保持し、CPostProcess::Render に 1 つ渡す。各メンバは既定値で
 * 「無効 or 中性」になるよう設計してある。
 */
struct FPostProcessParams {
    /** Bloom を有効にするか。 */
    bool  bloom_enabled    = true;

    /** この輝度を超えるピクセルを Bloom 元として抽出する閾値 (HDR scale)。 */
    f32   bloom_threshold  = 1.0f;

    /** Bloom を加算する強度 (0..2)。 */
    f32   bloom_intensity  = 0.6f;

    /** upsample 時の半径スケール。 */
    f32   bloom_radius     = 1.0f;

    /**
     * 隣接 mip 間の散乱率 (0=局所的、1=最広域)。
     *
     * @details 各段を等重みで加算せず、正規化した補間で合成する。0.65 前後で
     * 発光体の芯を残しつつ、白い霧のようなエネルギー増幅を防ぐ。
     */
    f32   bloom_scatter    = 0.65f;

    /** 露出 (1.0 = 中性)。 */
    f32   exposure         = 1.0f;

    /** 出力ガンマ。 */
    f32   gamma            = 2.2f;

    /**
     * Tonemap カーブの種類。
     *
     * @details
     * 0=ACES Filmic (Narkowicz)、1=AgX (Sobotka)、2=Reinhard 拡張。AgX は彩度を
     * 控えめにする tonemap (UE5 デフォルトに近い neutral look)。既存サンプル互換の
     * ため初期値は ACES。
     */
    i32   tonemap_kind     = 0;

    /** ビネット (端の暗化) 強度 0..1。 */
    f32   vignette_intensity = 0.2f;

    /** ビネットが始まる半径 (0=中心、1=端)。 */
    f32   vignette_radius    = 0.5f;

    /** 色収差: RGB の半径方向 offset。0 で無効。 */
    f32   chromatic_aberration = 0.002f;

    /** フィルムグレイン強度 0..0.1。 */
    f32   grain_intensity    = 0.015f;

    /** procedural noise のシード (FApplication から dt 累積)。 */
    f32   grain_time         = 0.0f;

    /**
     * tonemap 直前に additive 合成する SSR 出力テクスチャ (CSsr::OutputTexture())。
     *
     * @details null で SSR 無し。入力は scene-linear HDR とする。
     * auto exposure の完了値と manual exposure / EV compensation は
     * main HDR と同じく各 1 回だけ適用される。intensity は SSR shader 側で
     * 適用済みのため二重適用はされない。null 時は追加の exposure sample も発行しない。
     */
    IRhiTexture* ssr_texture = nullptr;

    /** tonemap 側での SSR の最終 mix 強度 (既定 1.0)。 */
    f32          ssr_intensity = 1.0f;

    /** カラーグレーディング (ASC-CDL 風) の彩度: 0=モノクロ、1=neutral、>1=ブースト。 */
    f32  cg_saturation   = 1.0f;

    /** カラーグレーディングのコントラスト: 0..2、中心 0.5 を pivot とした curve。 */
    f32  cg_contrast     = 1.0f;

    /** 色温度: -1=cool/blue、0=neutral、+1=warm/orange。 */
    f32  cg_temperature  = 0.0f;

    /** 色合い (tint): -1=green、0=neutral、+1=magenta。 */
    f32  cg_tint         = 0.0f;

    /** shadow 域の色 offset (足し算)。 */
    FVec3 cg_lift         = FVec3{0, 0, 0};

    /** highlight 域の色 multiplier。 */
    FVec3 cg_gain         = FVec3{1, 1, 1};

    /**
     * CAS シャープニング (AMD FSR 簡略版) の強度。
     *
     * @details カラーグレーディング後 / gamma 前に適用。0=無効、0.3=subtle、0.6=neutral、
     * 1.0=strong。負値は不可。既定で subtle に効かせ、見かけの解像感を上げる
     * (HDR-aware なので白飛び域でも破綻しない)。0 にすれば従来どおり無効。
     */
    f32  cas_strength    = 0.3f;

    /**
     * TAA (Temporal Anti-Aliasing) を有効にするか。
     *
     * @details
     * Halton jitter を FCamera で適用した上で、history と neighborhood-clamp blend して
     * resolve する。false なら tonemap は直接 HDR RT を読み、true なら Pass_TaaResolve を
     * 実行して tonemap は resolved RT を読む。
     */
    bool taa_enabled = false;

    /**
     * history 重み (現フレームを何割取り込むか)。
     *
     * @details 0.1 = 10% current + 90% history が標準。値が小さいほどスムージングが強い
     * (motion ghost も増える)。
     */
    f32  taa_blend_factor = 0.1f;

    /**
     * TAA reprojection 用の depth テクスチャ。
     *
     * @details null なら motion=0 (静的 reprojection 無し、カメラを動かすと ghost する)。
     * set すると camera motion 由来の motion vector で history を offset sample する。
     */
    IRhiTexture* taa_depth_texture = nullptr;

    /** Halton 適用前の view_proj 行列 (TAA reprojection 用)。 */
    FMat4         taa_view_proj_no_jitter{};

    /** 前フレームの view_proj 行列 (Halton 適用前、TAA reprojection 用)。 */
    FMat4         taa_prev_view_proj_no_jitter{};

    /** reactive cloud depth と scene depth の実距離比較に使う現在カメラ位置。 */
    FVec3         taa_camera_position{};

    /**
     * 動的 mesh 対応の motion vector テクスチャ (CMotionVector モジュール)。
     *
     * @details
     * 非 null なら TAA は depth reprojection の代わりにこのテクスチャで history を引く。
     * motion vector は camera 動き + object 動きの両方を含むため、動く mesh の ghost/trail が
     * 消える。null なら従来の depth reprojection にフォールバックする。depth slot (t2) を
     * 再利用して bind するため TAA resolve PSO の slot 数は不変。
     */
    IRhiTexture* taa_motion_texture = nullptr;

    /**
     * TAA history を現在フレームへ置換する reactive mask。
     *
     * @details RG の R 成分をカメラからの距離、G 成分を coverage として読む。
     * taa_depth_texture と taa_camera_position で scene の実距離を復元し、scene より
     * 手前に見えている coverage だけを reactive とする。ボリューメトリック雲のように
     * 独自の temporal resolve を済ませた要素を渡すと、その画素と 1 px の境界帯では
     * global TAA history を混ぜない。これによりジオメトリの TAA は維持しつつ、雲へ
     * 二重に history を掛ける ghost/trail を防ぐ。null なら reactive mask は無効。
     */
    IRhiTexture* taa_reactive_texture = nullptr;

    /**
     * Auto-exposure を有効にするか。
     *
     * @details
     * false なら exposure をそのまま使う (既存サンプル互換)。true なら luma reduction →
     * 露出順応 → ExposureApply の 3 pass を内部で実行し、露出はシーン輝度から自動算出される。
     * このとき exposure は「自動露出にさらに掛ける手動補正 (EV compensation)」として働く
     * (中性 = 1.0)。
     */
    bool auto_exposure_enabled = false;

    /** 露出後の目標平均輝度。大きいほど明るく写る。 */
    f32  auto_exposure_key     = 0.5f;

    /** 自動露出の下限 (明所での白飛びを防ぐ)。 */
    f32  auto_exposure_min     = 0.05f;

    /** 自動露出の上限 (暗所での黒つぶれを防ぐ)。 */
    f32  auto_exposure_max     = 12.0f;

    /** eye adaptation 速度 (/秒、大きいほど速く順応)。 */
    f32  auto_exposure_speed   = 1.8f;

    /** 露出順応の時間補間に使うフレーム時間。 */
    f32  delta_time            = 0.0166f;

    /**
     * Replaces non-finite values with defaults and clamps bounded controls to
     * the ranges accepted by the post-process shaders.
     *
     * @details Render() sanitizes a local copy automatically. Editor/property
     * systems may call this method to normalize values before displaying them.
     */
    void Sanitize() noexcept;
};

/**
 * HDR ポストプロセス (Bloom + Tonemap + 各種効果) パイプライン。
 *
 * @details
 * シーンを HDR R16G16B16A16_Float RT に描画した後、Bloom (extract → downsample →
 * upsample) → 任意の TAA resolve → Tonemap (ACES/AgX/Reinhard) → backbuffer の順に
 * 処理する。auto-exposure 有効時は luma 測定と露出順応 pass を追加で挟む。Diligent
 * backend を前提とし、GPU リソースを単独所有する non-copy 型。
 */
class CPostProcess {
public:
    /** Compiled shader handles awaiting owner-thread resource creation. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> fullscreen_vertex;
        TUniquePtr<IRhiShader> extract_pixel;
        TUniquePtr<IRhiShader> downsample_pixel;
        TUniquePtr<IRhiShader> upsample_pixel;
        TUniquePtr<IRhiShader> gaussian_pixel;
        TUniquePtr<IRhiShader> taa_resolve_pixel;
        TUniquePtr<IRhiShader> tonemap_pixel;
        TUniquePtr<IRhiShader> luma_extract_pixel;
        TUniquePtr<IRhiShader> luma_downsample_pixel;
        TUniquePtr<IRhiShader> exposure_pixel;
        TUniquePtr<IRhiShader> exposure_apply_pixel;

        /** Aggregate all eleven submitted shader jobs without waiting. */
        EShaderStatus Status() const noexcept;
    };

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CPostProcess() noexcept = default;

    /** 破棄する (確保した GPU リソースを解放)。 */
    ~CPostProcess() noexcept;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CPostProcess(const CPostProcess&) = delete;

    /** コピー代入も禁止。 */
    CPostProcess& operator=(const CPostProcess&) = delete;

    /**
     * HDR RT + Bloom mip chain + Tonemap パイプラインを作成する。
     *
     * @param device リソース・パイプライン生成に使う RHI デバイス。
     * @param width 出力解像度の幅 (通常はバックバッファサイズ)。
     * @param height 出力解像度の高さ。
     * @param color_format 最終出力 (バックバッファ) のフォーマット。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, u32 width, u32 height,
                       EFormat color_format) noexcept;

    /**
     * Compile all raw-DX12 post-process shaders without touching a device.
     *
     * @details Safe to execute on a background worker. GPU resources and PSOs
     * must still be created through InitWithCompiledShaders on the render
     * owner thread.
     */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /** Submit all eleven shaders to a backend-managed compiler pool. */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(
        IRhiDevice& device) noexcept;

    /**
     * Create resources and PSOs from ready shaders on the render owner thread.
     *
     * @details Reinitialization is transactional: a failure leaves an already
     * initialized post-process stack unchanged.
     */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        u32 width,
        u32 height,
        EFormat color_format) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * Keep allocated TAA targets but reject the previous logical camera's
     * output. The next enabled resolve starts from current color.
     */
    void InvalidateTaaHistory() noexcept {
        m_TaaFrame = 0u;
        m_TaaOutputValid = false;
    }

    /**
     * Keep exposure resources but reject adaptation from the previous logical
     * scene or camera. The next enabled auto-exposure frame meters current HDR
     * and takes its target exposure without blending stale eye adaptation.
     */
    void InvalidateExposureHistory() noexcept {
        m_AutoFrame = 0u;
        m_ExposureOutputValid = false;
    }

    /**
     * ウィンドウサイズ変更時に HDR RT 等を再作成する。
     *
     * @param width 新しい出力幅。
     * @param height 新しい出力高さ。
     * @return 成功なら空の TResult、再確保失敗ならエラー。
     *
     * @details All replacement render targets are created transactionally.
     * On failure the previous dimensions, targets, and temporal/exposure
     * history remain published and a later frame may retry.
     */
    TResult<void> Resize(u32 width, u32 height) noexcept;

    /**
     * シーンを描画する HDR RT を返す (CRenderer がここに描画する)。
     *
     * @return HDR R16G16B16A16_Float のレンダーターゲット。
     */
    IRhiTexture* HdrRenderTarget() const noexcept { return m_HdrRt.Get(); }

    /**
     * HDR RT のフォーマットを返す。
     *
     * @return HDR RT のフォーマット (既定 R16G16B16A16_Float)。
     */
    EFormat       HdrFormat()       const noexcept { return m_HdrFormat; }

    /**
     * 現在の PostProcess graph で利用できる transient alias 候補集計を返す。
     *
     * @details Resize 成功時にも更新される。実 GPU alias はバックエンドが
     * alias barrier と placed resource を提供する場合だけ、この安全な候補を使う。
     */
    const FRenderGraphAliasPlanSummary& TransientAliasPlan() const noexcept {
        return m_TransientAliasPlan;
    }

    /**
     * Bloom + Tonemap (+ 任意の TAA / auto-exposure) を実行し swapchain buffer へ書き出す。
     *
     * @param cmd 既に Begin 済みのコマンドリスト。
     * @param swapchain 出力先 (backbuffer をこのインスタンスから取り出す)。
     * @param buffer_index AcquireNextImage の戻り値。
     * @param params 適用する効果のパラメータ。
     */
    void Render(IRhiCommandList& cmd, IRhiSwapchain& swapchain, u32 buffer_index,
                const FPostProcessParams& params) noexcept;

private:
    CPostProcess& operator=(CPostProcess&&) noexcept = default;

    /**
     * Bloom mip chain の段数 (1/2 から 1/64 までの 6 段)。
     *
     * @details Downsample は Jimenez 13-tap。段数を増やすと «より低周波 (広い)» の soft glow まで
     * 届き、UE5 風の広く柔らかい bloom になる。段数増による強度 lift は progressive upsample radius
     * (深い mip ほど tent を広げる) と bloom_intensity 側で吸収する。各 mip は 1px までクランプ確保。
     */
    static constexpr u32 kBloomMips = 6;

    /**
     * HDR RT と Bloom mip chain を生成する。
     *
     * @param device RT 生成に使う RHI デバイス。
     * @param w 出力幅。
     * @param h 出力高さ。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreateRenderTargets(IRhiDevice& device, u32 w, u32 h) noexcept;

    /**
     * 全パスのシェーダとパイプラインを生成する。
     *
     * @param device パイプライン生成に使う RHI デバイス。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreatePipelines(
        IRhiDevice& device,
        FCompiledShaders&& shaders) noexcept;

    /**
     * Bloom 抽出パス: シーン入力から閾値超えの輝度を bloom_mips[0] へ書く。
     *
     * @param cmd コマンドを積むコマンドリスト。
     * @param p 適用する効果のパラメータ。
     */
    bool Pass_Extract  (IRhiCommandList& cmd, const FPostProcessParams& p) noexcept;

    /**
     * Bloom downsample パス: bloom_mips[from_mip] を次段へ 13-tap で縮約する。
     *
     * @param cmd コマンドを積むコマンドリスト。
     * @param from_mip 入力元の mip 段。
     */
    bool Pass_Downsample(IRhiCommandList& cmd, u32 from_mip) noexcept;

    /**
     * Bloom upsample パス: 下段を 1 段上へ additive 合成する。
     *
     * @param cmd コマンドを積むコマンドリスト。
     * @param to_mip 合成先の mip 段。
     * @param radius upsample 時の半径スケール。
     */
    bool Pass_Upsample (IRhiCommandList& cmd, u32 to_mip, f32 radius, f32 scatter) noexcept;

    /**
     * Bloom separable Gaussian blur: 1 つの mip を H or V 方向に 1 次元ガウスぼかしする。
     * H パス (mip→tmp) と V パス (tmp→mip) を続けて呼ぶと «円形» の 2D ガウスになる。
     * box mip チェーンと違い点光源が四角くならない (DirectXTK 風)。
     *
     * @param cmd コマンドリスト。
     * @param mip 対象 mip 段 (m_BloomMips[mip] ↔ m_BloomTmp[mip])。
     * @param horizontal true=水平パス (mip→tmp)、false=垂直パス (tmp→mip)。
     * @param amount ガウスの広がり (texel 単位の step スケール)。
     */
    bool Pass_GaussianBlur(IRhiCommandList& cmd, u32 mip, bool horizontal, f32 amount) noexcept;

    /**
     * TAA resolve パス: 現フレームと history を neighborhood-clamp blend する。
     *
     * @param cmd コマンドを積むコマンドリスト。
     * @param p 適用する効果のパラメータ。
     */
    bool Pass_TaaResolve(IRhiCommandList& cmd, const FPostProcessParams& p) noexcept;

    /**
     * Tonemap パス: HDR + Bloom を合成し tonemap して backbuffer へ書く。
     *
     * @param cmd コマンドを積むコマンドリスト。
     * @param sc 出力先スワップチェーン。
     * @param buf_idx 書き出すバックバッファの index。
     * @param p 適用する効果のパラメータ。
     */
    bool Pass_Tonemap  (IRhiCommandList& cmd, IRhiSwapchain& sc, u32 buf_idx,
                        const FPostProcessParams& p) noexcept;

    /**
     * Auto-exposure: HDR を log2 輝度の mip chain に縮約する luma reduction パス。
     *
     * @param cmd コマンドを積むコマンドリスト。
     */
    bool Pass_LumaReduce  (IRhiCommandList& cmd) noexcept;

    /**
     * Auto-exposure: 測定した平均輝度から目標露出へ eye adaptation で順応させる。
     *
     * @param cmd コマンドを積むコマンドリスト。
     * @param p 適用する効果のパラメータ。
     */
    bool Pass_ExposureAdapt(IRhiCommandList& cmd, const FPostProcessParams& p) noexcept;

    /**
     * Apply the adapted exposure after the scene-linear temporal resolve.
     *
     * @param cmd Recording command list.
     * @param source Scene-linear HDR. This is the current TAA resolve when
     * TAA is enabled, otherwise the raw scene target.
     */
    bool Pass_ExposureApply(IRhiCommandList& cmd,
                            IRhiTexture& source) noexcept;

    /**
     * 下流パス (TAA / Bloom / Tonemap) が読むシーン texture を返す。
     *
     * @param p 適用する効果のパラメータ。
     * @return The exposure-applied image when auto exposure is enabled, the
     * scene-linear TAA resolve when TAA is enabled, or the raw HDR scene.
     */
    IRhiTexture* SceneInput(const FPostProcessParams& p) const noexcept;

    /**
     * 同一フレーム内の各 fullscreen draw 専用 Post CB を取得する。
     *
     * @details Raw DX12 の cpu_writable buffer はフレーム間のみリング化されるため、同じ
     * buffer を複数パスで Update すると、実行時に全 draw が最後の値を読む。各 draw に
     * 別 resource を割り当てて GPU address を固定する。
     * @return 次の CB。1 フレームの上限を超えた場合は null。
     */
    IRhiBuffer* AcquirePostCb() noexcept;

    /** Init で受け取った device (Resize で再利用)。 */
    IRhiDevice* m_Device = nullptr;

    /** 出力解像度の幅。 */
    u32         m_Width  = 0;

    /** 出力解像度の高さ。 */
    u32         m_Height = 0;

    /** 最終出力 (バックバッファ) のフォーマット。 */
    EFormat      m_ColorFormat = EFormat::B8G8R8A8_UNorm;

    /** HDR RT のフォーマット。 */
    EFormat      m_HdrFormat   = EFormat::R16G16B16A16_Float;

    /** メイン HDR RT (シーン描画先)。 */
    TUniquePtr<IRhiTexture> m_HdrRt;

    /** Bloom mip chain (各段は HDR、解像度は半分ずつ)。 */
    TUniquePtr<IRhiTexture> m_BloomMips[kBloomMips];

    /** Bloom separable Gaussian の ping-pong 用テンポラリ (各 mip と同サイズ)。 */
    TUniquePtr<IRhiTexture> m_BloomTmp[kBloomMips];

    /** 共通の全画面三角形 VS。 */
    TUniquePtr<IRhiShader>   m_VsFullscreen;

    /** Bloom 抽出パスのピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_PsExtract;

    /** Bloom downsample パスのピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_PsDownsample;

    /** Bloom upsample パスのピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_PsUpsample;

    /** Bloom separable Gaussian blur パスのピクセルシェーダ (DirectXTK 風、H/V 2 パスで円形)。 */
    TUniquePtr<IRhiShader>   m_PsGaussian;

    /** Tonemap パスのピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_PsTonemap;

    /** Bloom 抽出パイプライン (HDR → bloom_mips[0])。 */
    TUniquePtr<IRhiPipeline> m_PipeExtract;

    /** Bloom downsample パイプライン (bloom_mips[i] → bloom_mips[i+1])。 */
    TUniquePtr<IRhiPipeline> m_PipeDownsample;

    /** Bloom upsample パイプライン (bloom_mips[i+1] + bloom_mips[i] → bloom_mips[i])。 */
    TUniquePtr<IRhiPipeline> m_PipeUpsample;

    /** Bloom Gaussian blur パイプライン (separable、Opaque)。 */
    TUniquePtr<IRhiPipeline> m_PipeGaussian;

    /** Tonemap パイプライン (HDR + bloom_mips[0] → backbuffer)。 */
    TUniquePtr<IRhiPipeline> m_PipeTonemap;

    /** True only after every bloom stage for the current frame was recorded. */
    bool                     m_BloomOutputValid = false;

    /**
     * 各 fullscreen draw のパラメータを固定する Post CB ring。
     *
     * @details 最大構成は luma 13 + TAA 1 + bloom 14 + tonemap 1 draw。将来の
     * パス追加にも余裕を持たせて 64 本確保し、Render 冒頭で cursor を戻す。
     */
    static constexpr u32     kPostCbRing = 64;
    TUniquePtr<IRhiBuffer>   m_CbPost[kPostCbRing];
    u32                      m_PostCbCursor = 0;

    /**
     * TAA の ping-pong history RT。
     *
     * @details m_Taa[N%2] が current frame の resolved 出力、m_Taa[(N+1)%2] が previous frame の
     * resolved 出力 (= history input)。frame index で role を swap する。
     */
    TUniquePtr<IRhiTexture>  m_Taa[2];

    /** TAA resolve パスのピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_PsTaaResolve;

    /** TAA resolve パイプライン。 */
    TUniquePtr<IRhiPipeline> m_PipeTaaResolve;

    /** TAA の現フレーム index (history ping-pong 用)。 */
    u32                     m_TaaFrame = 0;

    /** Prevents an incomplete resolve from publishing an old/unwritten RT. */
    bool                    m_TaaOutputValid = false;

    /** TAA reprojection 用の行列 2 枚を渡す CB (separate b1)。 */
    TUniquePtr<IRhiBuffer>   m_CbTaaReproj;

    /** depth 未指定時の fallback depth テクスチャ (1x1)。 */
    TUniquePtr<IRhiTexture>  m_TaaDepthFb;

    /** Bloom / SSR が無い texture slot に必ず bind する 1x1 黒テクスチャ。 */
    TUniquePtr<IRhiTexture>  m_BlackFb;

    /**
     * Auto-exposure の最大 luma mip 段数。
     *
     * @details 4K (3840) でも floor(log2)+1 に収まる値。
     */
    static constexpr u32 kMaxLumaMips = 13;

    /**
     * 平均輝度測定用の luma mip chain。
     *
     * @details m_HdrRt の 1/2 から 1x1 まで縮約する。最深段 (1x1) にシーン平均 log2 輝度が入る。
     */
    TUniquePtr<IRhiTexture>  m_LumaMips[kMaxLumaMips];

    /** 実際に確保した luma mip の段数。 */
    u32                     m_LumaMipCount = 0;

    /**
     * 順応済み露出を保持する 1x1 ping-pong テクスチャ。
     *
     * @details frame N が m_Exposure[N%2] に書き、m_Exposure[(N+1)%2] = 前フレーム値を読む。
     */
    TUniquePtr<IRhiTexture>  m_Exposure[2];

    /** m_HdrRt に自動露出を掛けた結果 (下流パスはこれを読む)。 */
    TUniquePtr<IRhiTexture>  m_ExposedRt;

    /** HDR → log2 輝度の抽出 (downsample 兼) ピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_PsLumaExtract;

    /** log2 輝度を box average で縮約するピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_PsLumaDown;

    /** 露出順応 (eye adaptation) のピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_PsExposure;

    /** m_HdrRt * 露出 → m_ExposedRt のピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_PsExposeApply;

    /** luma 抽出パイプライン。 */
    TUniquePtr<IRhiPipeline> m_PipeLumaExtract;

    /** luma downsample パイプライン。 */
    TUniquePtr<IRhiPipeline> m_PipeLumaDown;

    /** 露出順応パイプライン。 */
    TUniquePtr<IRhiPipeline> m_PipeExposure;

    /** 露出適用パイプライン。 */
    TUniquePtr<IRhiPipeline> m_PipeExposeApply;

    /** auto-exposure 用パラメータ CB。 */
    TUniquePtr<IRhiBuffer>   m_CbAuto;

    /** 露出 ping-pong / cold-start 判定に使うフレームカウンタ。 */
    u32                     m_AutoFrame = 0;

    /** True only when metering, adaptation and apply completed this frame. */
    bool                    m_ExposureOutputValid = false;

    /** luma mip / 露出テクスチャのフォーマット。 */
    EFormat                  m_LumaFormat = EFormat::R16G16_Float;

    /** 固定 PostProcess graph から計算した transient alias 候補集計。 */
    FRenderGraphAliasPlanSummary m_TransientAliasPlan{};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FPostProcess = CPostProcess;


} // namespace acs
