// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "math/Mat.h"      // FMat4 (BuildAerialPerspective の inv_view_proj)
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"

namespace acs {

// CPUとGPUで評価する物理大気散乱。Rayleigh散乱とMie散乱を方向ごとに評価し、
// 環境キューブ地図、拡散環境光、鏡面反射用事前計算へ同じ空の光を供給する。

/** 地球モデルの地表半径 (m)。CPU積分とGPU参照表で共通に使う。 */
inline constexpr f32 kSkyAtmosphereGroundRadiusMeters = 6360000.0f;
/** 大気を無視できる上端までの高度 (m)。 */
inline constexpr f32 kSkyAtmosphereTopAltitudeMeters = 100000.0f;
/** 地球中心から測った大気上端半径 (m)。 */
inline constexpr f32 kSkyAtmosphereTopRadiusMeters =
    kSkyAtmosphereGroundRadiusMeters + kSkyAtmosphereTopAltitudeMeters;

/** 確保処理と検証で共有する空気遠近法の体積表品質。 */
inline constexpr u32 kSkyAtmosphereFroxelXyResolution = 48u;
inline constexpr u32 kSkyAtmosphereFroxelZResolution = 96u;
inline constexpr u32 kSkyAtmosphereFroxelIntegrationSteps = 24u;
/** Near-field range reserved for the dedicated local-fog froxel volume. */
inline constexpr f32 kLocalVolumetricFogMaxDistance = 2500.0f;

/**
 * 大気散乱 bake の入力パラメータ (太陽方向・強度とサンプル数)。
 *
 * @details
 * sun_dir は天頂方向 +Y を基準とした太陽方角で、正規化されていなくてもよい
 * (BakeEquirect 内で正規化される)。ray_steps / sun_steps は単散乱積分の精度と
 * コストのトレードオフを決める。
 */
struct FAtmosphereParams {
    /** 太陽方角 (天頂方向 +Y、正規化前提だが内部で再正規化される)。 */
    FVec3 sun_dir       = FVec3{0.4f, 0.7f, 0.4f};

    /** 太陽のピーク輝度 (W/m²/sr 相当)。 */
    FVec3 sun_intensity = FVec3{22.0f, 22.0f, 22.0f};

    /** Lambert ground と ground bounce に使う RGB アルベド。 */
    FVec3 ground_albedo = FVec3{0.10f, 0.12f, 0.10f};

    /** view ray 沿いのサンプル数。 */
    u32  ray_steps     = 32;

    /** 各 sample から sun への光線 (透過率) でのサンプル数。 */
    u32  sun_steps     = 8;
};

/**
 * camera-volume LUT に統合するローカル height fog。
 *
 * @details density=0 ならローカル fog は無効で、大気の aerial perspective のみを積分する。
 * density は scene 単位あたりの extinction。色は単散乱 albedo、anisotropy は
 * Henyey-Greenstein 位相関数の g。
 */
struct FVolumetricFogParams {
    FVec3 color          = FVec3{0.62f, 0.70f, 0.82f};
    f32   density        = 0.0f;
    f32   height_falloff = 0.10f;
    f32   height_base    = 0.0f;
    f32   anisotropy     = 0.40f;
    f32   sun_scatter    = 0.18f;
};

/**
 * 物理大気散乱を CPU で評価し equirect 画像へ焼くユーティリティ。
 *
 * @details
 * Rayleigh + Mie 単散乱を per-direction で評価し、
 * 焼いた equirect 画像を CImageBasedLighting に渡すと env cubemap → irradiance →
 * prefilter の IBL chain が物理ベースの sky で構築できる。
 */
/**
 * 指定した高度で、太陽方向へ抜ける大気透過率を返す (CPU、LUT 不要)。
 *
 * @details
 * 雲や遠景を照らす太陽の «色» に使う。低い太陽ほど青が削られて赤くなるので、これを
 * 掛けないと夕方でも雲が昼の白さのままになる。水平位置には依らないものとして扱う。
 * @param altitude 地表からの高さ (world 単位)。
 * @param sun_dir 太陽へ向かう方向 (上が +Y、正規化は内部で行う)。
 * @return RGB 透過率。地面に隠れる向きなら 0。
 */
FVec3 SunTransmittanceAtAltitude(f32 altitude, FVec3 sun_dir) noexcept;

class CAtmosphere {
public:
    /**
     * 指定高度・指定方向の空の放射輝度を、BakeEquirect と同じ物理積分で返す。
     *
     * @details Rayleigh散乱、Mie散乱、オゾン吸収、太陽光の大気透過を同じ区間積分で
     * 評価する。空だけでなく地面へ当たる方向は、地表反射と視線透過を連続的に返す。
     * 雲や水面の環境照明が、表示中の空と別の固定色にならないように使う。
     * @param altitude 地表からの高度 (m)。負値は地表へ丸める。
     * @param view_dir 観察方向。正規化されていなくてもよい。
     * @param params 太陽方向・強度、地表アルベド、積分段数。
     * @return 指定方向の線形HDR放射輝度。入力が退化しても有限値を返す。
     */
    static FVec3 EvaluateSkyRadiance(f32 altitude, FVec3 view_dir,
                                     const FAtmosphereParams& params) noexcept;

    /**
     * CPU で equirect 画像を焼いて RGBA float 配列を返す。
     *
     * @details
     * 出力は w*h*4 個の float で上から下へ並び、v=0 が +Y 天頂、v=1 が -Y 天底
     * (sIBL Archive 規約と一致)。解析的な太陽ディスクは低解像度の環境テクスチャへ
     * 焼き込まず、最終 skybox pass で描画する。戻り値の TArray は move で呼び出し側に渡される。
     * @param width 焼く equirect 画像の幅 (ピクセル)。
     * @param height 焼く equirect 画像の高さ (ピクセル)。
     * @param params 太陽方向・強度とサンプル数を含む bake パラメータ。
     * @return RGBA float を格納した TArray (w*h*4 要素)。
     */
    static TArray<f32> BakeEquirect(u32 width, u32 height,
                                    const FAtmosphereParams& params) noexcept;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FAtmosphere = CAtmosphere;


/**
 * GPU の計算処理で参照表を構築する物理大気。
 *
 * @details 透過率表 (256x64) を計算処理で焼き、正距円筒画像の生成処理がそれを使って
 * Rayleigh+Mie+ozone の単散乱 + 等方多重散乱を per-direction で評価して equirect texture
 * (RGBA32F、解析的な太陽ディスクを含まない) に書く。ReadTexture で CPU へ読み戻し、
 * 結果を CImageBasedLighting::LoadEquirectHdrFromMemory に通せば既存の
 * env cubemap → irradiance → prefilter の IBL chain と背景描画がそのまま動く。
 * CPU 版 CAtmosphere::BakeEquirect の置き換え (GPU で高速 + ozone/multiscatter で物理的に正しい空)。
 * compute コア + CDiligentDevice::ReadTexture が必要。Diligent backend 専用。
 */
class CSkyAtmosphere {
public:
    /** compute パイプライン (transmittance / equirect bake) と Transmittance LUT・CB を生成。 */
    TResult<void> Init(IRhiDevice& device,
                       EFormat hdr_format = EFormat::R16G16B16A16_Float) noexcept;

    /** 初期化済みか。 */
    bool Ready() const noexcept { return m_Ready; }

    /**
     * GPU で大気 equirect を焼き CPU の RGBA float 配列へ読み戻す (LoadEquirectHdrFromMemory 互換)。
     *
     * @param device RHI デバイス。
     * @param cl コマンドリスト。
     * @param params 太陽方向・強度。
     * @param width  equirect 幅。
     * @param height equirect 高さ。
     * @param out    出力 (width*height*4 個の f32、move せず resize して埋める)。
     * @return 成功で true (失敗時 out は不定、呼び出し側で CPU fallback)。
     */
    bool BakeEquirect(IRhiDevice& device, IRhiCommandList& cl,
                      const FAtmosphereParams& params,
                      u32 width, u32 height, TArray<f32>& out) noexcept;

    /**
     * Aerial perspective の camera-volume LUT を焼いて返す。
     *
     * @details
     * 48x48x96 froxel の各セルに camera→距離までの大気を積分する。
     * m_ApVol は premultiplied in-scatter、m_ApTransVol は RGB transmittance を保持し、
     * screen uv + 深度→スライスで scene.rgb = scene.rgb*T.rgb + L.rgb として適用する。
     * 3D LUT を物理積分するため滑らか (cubemap サンプルのような «斜めの段» が出ない)。
     * @param inv_view_proj 逆 view-projection (froxel の world ray 復元用)。
     * @param cam_pos カメラ world position (scene 単位)。
     * @param sun_dir 太陽方角 (+Y up)。
     * @param sun_intensity 太陽ピーク輝度。
     * @param max_dist_scene volume がカバーする最大距離 (scene 単位)。
     * @param scene_to_km scene 単位 → 大気 km の換算 (見た目調整。小さいシーンで霞を可視化)。
     * @param cam_alt_km カメラの大気高度 (km、地表 ≈ 0)。
     * @return AP volume (失敗時または scene_to_km<=0 の無効時は nullptr、非所有)。
     */
    IRhiTexture* BuildAerialPerspective(IRhiDevice& device, IRhiCommandList& cl,
                                        const FMat4& inv_view_proj, FVec3 cam_pos,
                                        FVec3 sun_dir, FVec3 sun_intensity,
                                        f32 max_dist_scene, f32 scene_to_km,
                                        f32 cam_alt_km) noexcept;

    /**
     * ローカル volumetric fog を同じ camera-volume に統合するオーバーロード。
     *
     * @param fog ローカル volumetric height fog (density=0 で無効)。
     */
    IRhiTexture* BuildAerialPerspective(IRhiDevice& device, IRhiCommandList& cl,
                                        const FMat4& inv_view_proj, FVec3 cam_pos,
                                        FVec3 sun_dir, FVec3 sun_intensity,
                                        f32 max_dist_scene, f32 scene_to_km,
                                        f32 cam_alt_km,
                                        const FVolumetricFogParams& fog) noexcept;

    /**
     * カメラ相対逆行列から空気遠近法と局所霧の体積表を高精度に作る。
     *
     * @param camera_relative_inv_view_proj 平行移動を含まない逆ビュープロジェクション行列。
     * @param cam_pos 局所霧の絶対高度を求めるカメラのワールド位置。
     * @return 物理大気が有効なら空気遠近法の体積表、無効または失敗ならnullptr。
     */
    IRhiTexture* BuildAerialPerspectiveCameraRelative(IRhiDevice& device, IRhiCommandList& cl, const FMat4& camera_relative_inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_intensity, f32 max_dist_scene, f32 scene_to_km, f32 cam_alt_km) noexcept;

    /** 局所霧の設定も受け取るカメラ相対版。 */
    IRhiTexture* BuildAerialPerspectiveCameraRelative(IRhiDevice& device, IRhiCommandList& cl, const FMat4& camera_relative_inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_intensity, f32 max_dist_scene, f32 scene_to_km, f32 cam_alt_km, const FVolumetricFogParams& fog) noexcept;

    /** 直近に焼いた AP volume (BuildAerialPerspective 後に有効、非所有)。 */
    IRhiTexture* ApVolume() const noexcept { return m_ApVol.Get(); }

    /** 直近に焼いた wavelength-dependent AP transmittance volume。 */
    IRhiTexture* ApTransmittanceVolume() const noexcept {
        return m_ApTransVol.Get();
    }

    /**
     * 直近に焼いた local-fog-only volume。
     *
     * @details BuildAerialPerspective に density>0 の fog を渡したフレームだけ有効。
     * Rayleigh/Mie は含まないため、既に物理大気を積分済みの clear sky に安全に使える。
     */
    IRhiTexture* LocalFogVolume() const noexcept {
        return m_LocalFogVolumeValid ? m_LocalFogVol.Get() : nullptr;
    }

    /** Scene-space range represented by LocalFogVolume(). */
    f32 LocalFogMaxDistance() const noexcept {
        return m_LocalFogMaxDistance;
    }

    /** Init 後に physical aerial-perspective volume を再焼成した累積回数。 */
    u64 PhysicalApDispatchCount() const noexcept {
        return m_PhysicalApDispatchCount;
    }

    /** Init 後に local-fog-only volume を再焼成した累積回数。 */
    u64 LocalFogDispatchCount() const noexcept {
        return m_LocalFogDispatchCount;
    }

    /**
     * 深度で camera-volume を終端し、現在の HDR render target へ一度だけ合成する。
     *
     * @details 呼び出し側は depth を DSV に bind せず、描画先だけを load した render pass
     * を開始しておくこと。RGB transmittance の乗算後に premultiplied in-scatter を
     * alpha を壊さない加算 pass で重ね、scene*T+in-scatter を再現する。
     */
    void CompositeAerialPerspective(IRhiCommandList& cl,
                                    IRhiTexture& depth,
                                    IRhiTexture& ap_volume,
                                    IRhiTexture& transmittance_volume,
                                    const FMat4& inv_view_proj,
                                    FVec3 cam_pos,
                                    f32 max_dist_scene,
                                    u32 screen_width,
                                    u32 screen_height) noexcept;

    /** カメラ相対逆行列で深度位置を復元し、遠方でも空気遠近法の距離精度を保つ。 */
    void CompositeAerialPerspectiveCameraRelative(IRhiCommandList& cl, IRhiTexture& depth, IRhiTexture& ap_volume, IRhiTexture& transmittance_volume, const FMat4& camera_relative_inv_view_proj, f32 max_dist_scene, u32 screen_width, u32 screen_height) noexcept;

    /**
     * Geometry と cleared-depth 背景へ local-fog-only volume を一度だけ合成する。
     *
     * @details Geometry は再構築した surface 距離で終端する。cloud_depth が有効なら
     * cleared-depth pixel も雲の実距離で終端し、純粋な sky のみ far slice を使う。
     * 物理大気は含まないため、空や雲へ重ねても二重散乱にならない。
     */
    void CompositeLocalFog(IRhiCommandList& cl,
                           IRhiTexture& depth,
                           IRhiTexture& local_fog_volume,
                           IRhiTexture* cloud_depth,
                           const FMat4& inv_view_proj,
                           FVec3 cam_pos,
                           f32 max_dist_scene,
                           u32 screen_width,
                           u32 screen_height) noexcept;

    /** カメラ相対逆行列で深度位置を復元し、局所霧を正しい視線距離で終端する。 */
    void CompositeLocalFogCameraRelative(IRhiCommandList& cl, IRhiTexture& depth, IRhiTexture& local_fog_volume, IRhiTexture* cloud_depth, const FMat4& camera_relative_inv_view_proj, f32 max_dist_scene, u32 screen_width, u32 screen_height) noexcept;

    /** 全 GPU リソースを解放 (acs_editor_destroy から呼ぶ。UAF 防止)。 */
    void Shutdown() noexcept;

private:
    struct FVolumeCacheKey {
        FMat4 invViewProj{};
        FVec4 camPos{};
        FVec4 sunDir{};
        FVec4 sunInt{};
        FVec4 apParams{};
        FVec4 fogColorDensity{};
        FVec4 fogParams{};
    };
    static_assert(sizeof(FVolumeCacheKey) == 160u,
                  "volume cache keys must not contain implicit padding");

    static bool SameVolumeCacheKey(const FVolumeCacheKey& lhs,
                                   const FVolumeCacheKey& rhs) noexcept;

    bool                     m_Ready = false;
    bool                     m_LutsReady = false;      // transmittance + multi-scattering は一度だけ焼く
    TUniquePtr<IRhiShader>   m_TransCs;
    TUniquePtr<IRhiShader>   m_BakeCs;
    TUniquePtr<IRhiPipeline> m_TransPipe;
    TUniquePtr<IRhiPipeline> m_BakePipe;
    TUniquePtr<IRhiTexture>  m_TransLut;    // 256x64 RGBA16F UAV/SRV
    TUniquePtr<IRhiShader>   m_MultiCs;     // 多重散乱 LUT CS (WE multiScatteredLuminanceLut)
    TUniquePtr<IRhiPipeline> m_MultiPipe;
    TUniquePtr<IRhiTexture>  m_MultiLut;    // 32x32 RGBA16F UAV/SRV (Fms)
    TUniquePtr<IRhiTexture>  m_Equirect;    // width x height RGBA32F UAV (readback source)
    TUniquePtr<IRhiBuffer>   m_Cb;          // sun dir + intensity + ground albedo
    TUniquePtr<IRhiShader>   m_ApCs;        // aerial perspective froxel CS
    TUniquePtr<IRhiPipeline> m_ApPipe;
    TUniquePtr<IRhiShader>   m_LocalFogCs;  // scalar local-fog-only froxel CS
    TUniquePtr<IRhiPipeline> m_LocalFogPipe;
    TUniquePtr<IRhiTexture>  m_ApVol;       // 48x48x96 RGBA16F premultiplied in-scatter
    TUniquePtr<IRhiTexture>  m_ApTransVol;  // 48x48x96 RGBA16F RGB transmittance
    TUniquePtr<IRhiBuffer>   m_ApCb;        // AP cbuffer (invVP/cam/sun/params)
    TUniquePtr<IRhiTexture>  m_LocalFogVol; // 同解像度の local-fog-only volume
    TUniquePtr<IRhiBuffer>   m_LocalFogCb;  // AP dispatch と同フレームに安全に使う専用 CB
    bool                     m_LocalFogVolumeValid = false;
    f32                      m_LocalFogMaxDistance = kLocalVolumetricFogMaxDistance;
    FVolumeCacheKey          m_PhysicalApCacheKey{};
    FVolumeCacheKey          m_LocalFogCacheKey{};
    bool                     m_PhysicalApCacheValid = false;
    bool                     m_LocalFogCacheValid = false;
    u64                      m_PhysicalApDispatchCount = 0;
    u64                      m_LocalFogDispatchCount = 0;
    TUniquePtr<IRhiShader>   m_ApCompositeVs;
    TUniquePtr<IRhiShader>   m_ApCompositePs;
    TUniquePtr<IRhiPipeline> m_ApCompositePipe;
    TUniquePtr<IRhiShader>   m_ApMultiplyPs;
    TUniquePtr<IRhiPipeline> m_ApMultiplyPipe;
    TUniquePtr<IRhiShader>   m_ApAddPs;
    TUniquePtr<IRhiPipeline> m_ApAddPipe;
    TUniquePtr<IRhiBuffer>   m_ApCompositeCb;       // physical AP draws only
    TUniquePtr<IRhiBuffer>   m_LocalFogCompositeCb; // later local-fog draw only
    u32                      m_EqW = 0, m_EqH = 0;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FSkyAtmosphere = CSkyAtmosphere;


} // namespace acs
