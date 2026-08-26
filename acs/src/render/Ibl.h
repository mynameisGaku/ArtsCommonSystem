// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiTexture.h"
#include "render/IRhiCommandList.h"
#include "render/RhiTypes.h"

namespace acs {

class CSky;

/**
 * Image-Based Lighting。PBR の ambient 項を環境マップから事前積分した光で置き換える。
 *
 * @details
 * 構成要素は BRDF LUT (256x256 RG16F、GGX split-sum の scale+bias)、環境 cubemap
 * (1024x1024x6 R11G11B10_Float)、拡散 irradiance cubemap (64x64x6)、specular prefilter
 * cubemap (512x512x6, 7 mip)。Diligent backend 専用の本実装で、raw-DX12 backend では
 * 各 Build/Ensure が ACS_ERR(Render, 88) を返す (fake-success しない)。
 */
class CImageBasedLighting {
public:
    /** 空状態で構築する (各 GPU リソースは Ensure 系で遅延生成)。 */
    CImageBasedLighting() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~CImageBasedLighting() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CImageBasedLighting(const CImageBasedLighting&)            = delete;

    /** コピー代入も禁止。 */
    CImageBasedLighting& operator=(const CImageBasedLighting&) = delete;

    /**
     * 初回呼び出しで BRDF LUT を 1 回だけ生成する (以降は no-op)。
     *
     * @details cl は frame 内 (BeginFrame と EndFrame の間) で記録中である必要がある。
     * @param device リソース生成に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> EnsureBrdfLut(IRhiDevice& device, IRhiCommandList& cl) noexcept;

    /**
     * 初回呼び出しで env cubemap を CSky 手続き式からキャプチャする。
     *
     * @details
     * 1024x1024x6 / R11G11B10_Float を各 face 6 回の per-slice draw で塗る。sky の現在の
     * パラメータ (sun_dir / colors 等) がスナップショットされる。再キャプチャするには
     * Shutdown() で全 reset するか ResetEnvCubemap() を使う。
     * @param device リソース生成に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @param sky キャプチャ元の手続き式空。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> EnsureEnvCubemap(IRhiDevice& device, IRhiCommandList& cl,
                                  const CSky& sky) noexcept;

    /**
     * equirectangular HDR画像から新しいenv cubemapを作り、成功時だけ既存環境を置き換える。
     *
     * @details
     * 内部でR32G32B32A32_FloatのTexture2Dへuploadし、6面をピクセルシェーダで塗る。
     * 生成途中で失敗した場合は既存のenv、irradiance、prefilterを維持する。成功時は
     * envだけを置き換えて派生地図を無効化するため、呼び出し側で再Ensureする必要がある。
     * @param device リソース生成に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @param rgba_float width × height × 4 個の float (row-major / top-down、v=0 が天頂 +Y、v=1 が天底 -Y)。
     * @param width 画像の幅 (px)。
     * @param height 画像の高さ (px)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> LoadEquirectHdrFromMemory(IRhiDevice& device, IRhiCommandList& cl,
                                            const f32* rgba_float, u32 width, u32 height) noexcept;

    /**
     * Y軸回転したequirectangular HDR画像から作り、成功時だけ既存環境を置き換える。
     *
     * @details 正の回転は画像内の+Z方向を+X方向へ移す。TrueHDRIでは同じ角度を
     * FTrueHdriLightData::ResolveSunDirectionへ渡すことで背景、間接光、平行光を一致させる。
     * 生成途中で失敗した場合は公開中のenv、irradiance、prefilterを変更しない。
     * 5引数版は回転角0度でこの関数を呼ぶ互換入口である。
     * @param device リソース生成に使うRHIデバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @param rgba_float width×height×4個の線形RGBA値。
     * @param width 画像の幅。
     * @param height 画像の高さ。
     * @param environment_yaw_degrees HDR画像へ加えるY軸回転角。正なら+Zから+Xへ回す。
     * @return 入力と回転角が有効で生成できれば成功、それ以外はエラー。
     */
    TResult<void> LoadEquirectHdrFromMemory(IRhiDevice& device, IRhiCommandList& cl, const f32* rgba_float, u32 width, u32 height, f32 environment_yaw_degrees) noexcept;

    /**
     * 環境用skyboxの太陽円盤を残し、拡散・鏡面IBLの畳み込みから解析的な直接光だけを除く。
     *
     * @details 有限個の標本で画素未満のHDR太陽を積分すると規則的な輝点になり、CPbrShaderも
     * 同じ太陽を方向光BRDFで評価する。無効化する場合はcosine_half_angleへ1より大きい値を渡す。
     * 環境を作り直すときはEnsureIrradiance／EnsurePrefilterより前に呼ぶ。生成済みのGPU資源は
     * 直ちに壊さないため、変更後はResetEnvCubemapを呼んでから再生成する。旧資源の寿命はRHIが
     * 送信完了まで保持するので、GPU全体の完了待ちは不要。
     */
    void SetDirectLightExclusion(FVec3 direction,
                                 f32 cosine_half_angle) noexcept;

    /**
     * equirect 画像から SH 9 light probe を CPU 側で計算する。
     *
     * @details
     * Ramamoorthi-Hanrahan の L_l,m 球面調和係数 9 個を求めて out_sh_rgb に書き出す。
     * 各 out_sh_rgb[i].xyz が RGB 係数で .w は不使用 (CB layout の便宜上 FVec4)。後段
     * CPbrShader で SH 9 ambient mode に切替でき、irradiance cubemap の圧縮版 (144B のみ)
     * として diffuse irradiance を近似する。規約: v=0 が +Y、v=1 が -Y、u=0 が phi=-π。
     * @param rgba_float equirect 画像の RGBA float ピクセル列。
     * @param width 画像の幅 (px)。
     * @param height 画像の高さ (px)。
     * @param out_sh_rgb 計算した SH 係数 9 個の書き出し先。
     */
    static void ComputeSh9FromEquirect(const f32* rgba_float,
                                        u32 width, u32 height,
                                        FVec4 out_sh_rgb[9]) noexcept;

    /**
     * 初回呼び出しで diffuse irradiance cubemap を env cubemap の半球積分から作る。
     *
     * @details
     * 64x64x6 / R11G11B10_Float。EnsureEnvCubemap が完了していないと失敗する。出力は
     * (1/π) ∫_Ω L_env(ω) (N·ω) dω で、Lambert diffuse の ambient 項として
     * diffuse = albedo * irradiance_cube.Sample(N) で使う。
     * @param device リソース生成に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> EnsureIrradiance(IRhiDevice& device, IRhiCommandList& cl) noexcept;

    /**
     * 初回呼び出しで specular prefilter cubemap を env cubemap の GGX 積分から作る。
     *
     * @details
     * 512x512x6 / R11G11B10_Float / 7 mips。各 mip が roughness 段階に対応する
     * (mip 0 = roughness 0 / 鏡面、mip 6 = roughness 1)。実行時 PBR specular は
     * F = F0 * lut.r + lut.g、specular = prefilter.SampleLevel(R, roughness*(mips-1)).rgb * F。
     * @param device リソース生成に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> EnsurePrefilter(IRhiDevice& device, IRhiCommandList& cl) noexcept;

    /**
     * 表示用 env cubemap を置き換えず、指定した環境から間接光mapだけを作り直す。
     *
     * @details 雲を一時的な環境cubemapへ描いた後、その形と照明をirradiance / prefilterへ
     * 反映するために使う。両方の候補が完成してから公開するため、途中で失敗した場合は
     * 直前の間接光地図を維持する。置換前の資源はRHIの完了フェンス付き遅延解放へ渡すため、
     * 描画スレッドでGPU全体の完了を待たない。
     * @param device 描画資源を作る装置。
     * @param cl コマンドを積むコマンドリスト。
     * @param environment 6面を持つ読み取り可能なcubemap。
     * @return 両方のmapを更新できれば成功。不正な入力または生成失敗ならエラー。
     */
    TResult<void> RebuildDerivedMapsFromEnvironment(
        IRhiDevice& device, IRhiCommandList& cl,
        IRhiTexture& environment) noexcept;

    /**
     * デバッグ用に任意の cubemap を fullscreen quad の skybox として現在の RT へ描く。
     *
     * @details
     * rt_format / depth_format は現在 bind 中の swapchain と一致させること。depth_test /
     * write は無効 (背景描画)。初回呼び出しで PSO/CB を lazy init し、env_cube / irradiance
     * / prefilter で同じ pipeline を共有する。
     * @param device リソース生成・描画に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @param cube 描画する cubemap テクスチャ。
     * @param view_proj 適用する view-projection 行列。
     * @param eye カメラ位置 (視線方向の算出に使う)。
     * @param rt_format 現在 bind 中の RT の色フォーマット。
     * @param depth_format 現在 bind 中の depth フォーマット。
     * @param mip_level サンプルする mip 段階 (prefilter で roughness 段階を選ぶとき 0..prefilter_mips-1、env/irradiance では 0)。
     */
    void DrawSkybox(IRhiDevice& device, IRhiCommandList& cl,
                    IRhiTexture& cube,
                    const FMat4& view_proj, FVec3 eye,
                    EFormat rt_format, EFormat depth_format,
                    f32 mip_level = 0.0f,
                    FVec3 sun_direction = FVec3{0.0f, 1.0f, 0.0f},
                    FVec3 sun_radiance = FVec3{},
                    f32 sun_angular_radius = 0.0f) noexcept;

    /**
     * カメラ相対逆行列から任意の環境キューブ地図を高精度に描く。
     *
     * @param camera_relative_inv_view_proj 平行移動を含まない逆ビュープロジェクション行列。
     */
    void DrawSkyboxCameraRelative(IRhiDevice& device, IRhiCommandList& cl, IRhiTexture& cube, const FMat4& camera_relative_inv_view_proj, EFormat rt_format, EFormat depth_format, f32 mip_level = 0.0f, FVec3 sun_direction = FVec3{0.0f, 1.0f, 0.0f}, FVec3 sun_radiance = FVec3{}, f32 sun_angular_radius = 0.0f) noexcept;

    /**
     * env_cube を skybox として現在の RT へ描く利便ラッパ。
     *
     * @param device リソース生成・描画に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @param view_proj 適用する view-projection 行列。
     * @param eye カメラ位置。
     * @param rt_format 現在 bind 中の RT の色フォーマット。
     * @param depth_format 現在 bind 中の depth フォーマット。
     * @param sun_direction 解析的に描く太陽への方向。正規化は内部で行う。
     * @param sun_radiance 太陽ディスクの線形 HDR 放射輝度。0 ならディスクを描かない。
     * @param sun_angular_radius 太陽の角半径 (rad)。0 ならディスクを描かない。
     *
     * @details
     * 太陽ディスクは env cubemap へ焼き込まず、最終ビューポート解像度で解析的に
     * 描画する。これにより低解像度 equirect のテクセル形状が四角く拡大されず、
     * 後段の volumetric cloud composite が自然にディスクを遮蔽できる。
     */
    void DrawEnvSkybox(IRhiDevice& device, IRhiCommandList& cl,
                       const FMat4& view_proj, FVec3 eye,
                       EFormat rt_format, EFormat depth_format,
                       FVec3 sun_direction = FVec3{0.0f, 1.0f, 0.0f},
                       FVec3 sun_radiance = FVec3{},
                       f32 sun_angular_radius = 0.0f) noexcept;

    /** 環境キューブ地図をカメラ相対逆行列から描く高精度版。 */
    void DrawEnvSkyboxCameraRelative(IRhiDevice& device, IRhiCommandList& cl, const FMat4& camera_relative_inv_view_proj, EFormat rt_format, EFormat depth_format, FVec3 sun_direction = FVec3{0.0f, 1.0f, 0.0f}, FVec3 sun_radiance = FVec3{}, f32 sun_angular_radius = 0.0f) noexcept;

    /**
     * 環境 cubemap (とそれに依存する irradiance / prefilter) だけを reset する。
     *
     * @details
     * CSky preset 切替などで env を作り直したいときに使い、BRDF LUT (sky 非依存) は残せる。
     * 解放した資源はRHIが現在の送信完了まで保持するため、呼び出し側のGPU全体待機は不要。
     */
    void ResetEnvCubemap() noexcept;

    /** 確保した全 GPU リソース (LUT・各 cubemap・skybox パイプライン) を解放する。 */
    void Shutdown() noexcept;

    /**
     * BRDF LUT を返す。
     *
     * @return BRDF LUT (256x256 RG16F)。EnsureBrdfLut 完了前は nullptr。
     */
    IRhiTexture* BrdfLut()    const noexcept { return m_BrdfLut.Get(); }

    /**
     * BRDF LUT が生成済みかを返す。
     *
     * @return 生成済みなら true。
     */
    bool         HasBrdfLut() const noexcept { return static_cast<bool>(m_BrdfLut); }

    /**
     * 環境 cubemap を返す。
     *
     * @return 環境 cubemap (1024x1024x6 R11G11B10_Float)。EnsureEnvCubemap 完了前は nullptr。
     */
    IRhiTexture* EnvCubemap()    const noexcept { return m_EnvCube.Get(); }

    /**
     * 環境 cubemap が生成済みかを返す。
     *
     * @return 生成済みなら true。
     */
    bool         HasEnvCubemap() const noexcept { return static_cast<bool>(m_EnvCube); }

    /**
     * 拡散 irradiance cubemap を返す。
     *
     * @return irradiance cubemap (64x64x6 R11G11B10_Float)。EnsureIrradiance 完了前は nullptr。
     */
    IRhiTexture* IrradianceMap()    const noexcept { return m_IrradianceCube.Get(); }

    /**
     * 拡散 irradiance cubemap が生成済みかを返す。
     *
     * @return 生成済みなら true。
     */
    bool         HasIrradianceMap() const noexcept { return static_cast<bool>(m_IrradianceCube); }

    /**
     * specular prefilter cubemap を返す。
     *
     * @return prefilter cubemap (512x512x6 R11G11B10_Float, 7 mips)。EnsurePrefilter 完了前は nullptr。
     */
    IRhiTexture* PrefilterMap()    const noexcept { return m_PrefilterCube.Get(); }

    /**
     * specular prefilter cubemap が生成済みかを返す。
     *
     * @return 生成済みなら true。
     */
    bool         HasPrefilterMap() const noexcept { return static_cast<bool>(m_PrefilterCube); }

    /**
     * prefilter cubemap の mip 数を返す。
     *
     * @return prefilter の mip 数 (未生成なら 0)。
     */
    u32          PrefilterMips()   const noexcept { return m_PrefilterMips; }

private:
    /**
     * BRDF LUT を実際に生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> BuildBrdfLut(IRhiDevice& device, IRhiCommandList& cl) noexcept;

    /**
     * env cubemap を実際に生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @param sky キャプチャ元の手続き式空。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> BuildEnvCubemap(IRhiDevice& device, IRhiCommandList& cl,
                                 const CSky& sky) noexcept;

    /**
     * irradiance cubemap を実際に生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> BuildIrradiance(
        IRhiDevice& device, IRhiCommandList& cl,
        IRhiTexture& environment,
        TUniquePtr<IRhiTexture>& output) noexcept;

    /**
     * prefilter cubemap を実際に生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> BuildPrefilter(
        IRhiDevice& device, IRhiCommandList& cl,
        IRhiTexture& environment,
        TUniquePtr<IRhiTexture>& output,
        u32& output_mips) noexcept;

    /**
     * skybox 描画用の PSO/CB を必要なら lazy init する。
     *
     * @param device パイプライン生成に使う RHI デバイス。
     * @param rt_format 描画先 RT の色フォーマット。
     * @param depth_format 描画先 depth フォーマット。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> EnsureSkyboxPipeline(IRhiDevice& device,
                                      EFormat rt_format, EFormat depth_format) noexcept;

    /** BRDF LUT (256x256 RG16F)。 */
    TUniquePtr<IRhiTexture>  m_BrdfLut;

    /** 環境 cubemap (1024x1024x6 R11G11B10_Float)。 */
    TUniquePtr<IRhiTexture>  m_EnvCube;

    /** 拡散 irradiance cubemap (64x64x6 R11G11B10_Float)。 */
    TUniquePtr<IRhiTexture>  m_IrradianceCube;

    /** specular prefilter cubemap (512x512x6 R11G11B10_Float, 7 mips)。 */
    TUniquePtr<IRhiTexture>  m_PrefilterCube;

    /** prefilter cubemap の mip 数。 */
    u32                     m_PrefilterMips = 0;

    /**
     * xyz = normalized direction toward the analytic direct light;
     * w = cosine of its exclusion half-angle.  w > 1 disables exclusion.
     */
    FVec4                    m_DirectLightExclusion =
        FVec4{0.0f, 1.0f, 0.0f, 2.0f};

    /** skybox preview の頂点シェーダ (lazy init)。 */
    TUniquePtr<IRhiShader>   m_SkyVs;

    /** skybox preview のピクセルシェーダ (lazy init)。 */
    TUniquePtr<IRhiShader>   m_SkyPs;

    /** skybox preview のパイプライン (lazy init)。 */
    TUniquePtr<IRhiPipeline> m_SkyPipeline;

    /** skybox preview の定数バッファ。 */
    TUniquePtr<IRhiBuffer>   m_SkyCb;

    /** skybox パイプライン生成時の RT フォーマット (再生成判定用)。 */
    EFormat                  m_SkyRtFormat    = EFormat::Unknown;

    /** skybox パイプライン生成時の depth フォーマット (再生成判定用)。 */
    EFormat                  m_SkyDepthFormat = EFormat::Unknown;

    /** BRDF LUT 生成済みフラグ。 */
    bool m_bBrdfBuilt       = false;

    /** env cubemap 生成済みフラグ。 */
    bool m_bEnvBuilt        = false;

    /** irradiance cubemap 生成済みフラグ。 */
    bool m_bIrradianceBuilt = false;

    /** prefilter cubemap 生成済みフラグ。 */
    bool m_bPrefilterBuilt  = false;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FImageBasedLighting = CImageBasedLighting;


} // namespace acs
