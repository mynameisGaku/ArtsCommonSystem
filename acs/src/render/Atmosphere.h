// SPDX-License-Identifier: Apache-2.0
// Physical atmospheric scattering (Hillaire 2020 / Bruneton 風)
//
// Rayleigh + Mie 単散乱を per-direction で CPU 評価し equirect 画像に焼く。
// `ImageBasedLighting::LoadEquirectHdrFromMemory` に通せば env cubemap →
// irradiance → prefilter の IBL chain が一気に物理ベースの sky で構築される。
//
// 物理パラメータ (Earth、Bruneton 2008):
//   - 地表半径 6360 km、大気上端 6420 km (厚さ 60 km)
//   - Rayleigh: β = (5.802, 13.558, 33.1) ×10⁻⁶ m⁻¹、scale height 8 km
//   - Mie:      β = 3.996 ×10⁻⁶ m⁻¹、absorption 4.4 ×10⁻⁶ m⁻¹、scale height 1.2 km、g=0.8
//
// 簡易: 単散乱のみ (multi-scatter / aerial perspective / ozone は未含)。
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

/**
 * 大気散乱 bake の入力パラメータ (太陽方向・強度とサンプル数)。
 *
 * @details
 * sun_dir は天頂方向 +Y を基準とした太陽方角で、正規化されていなくてもよい
 * (BakeEquirect 内で正規化される)。ray_steps / sun_steps は単散乱積分の精度と
 * コストのトレードオフを決める。
 */
struct AtmosphereParams {
    /** 太陽方角 (天頂方向 +Y、正規化前提だが内部で再正規化される)。 */
    FVec3 sun_dir       = FVec3{0.4f, 0.7f, 0.4f};

    /** 太陽のピーク輝度 (W/m²/sr 相当)。 */
    FVec3 sun_intensity = FVec3{22.0f, 22.0f, 22.0f};

    /** ground bounce 用アルベド (現状は未使用)。 */
    FVec3 ground_albedo = FVec3{0.10f, 0.12f, 0.10f};

    /** view ray 沿いのサンプル数。 */
    u32  ray_steps     = 32;

    /** 各 sample から sun への光線 (透過率) でのサンプル数。 */
    u32  sun_steps     = 8;
};

/**
 * 物理大気散乱を CPU で評価し equirect 画像へ焼くユーティリティ。
 *
 * @details
 * Hillaire 2020 / Bruneton 風の Rayleigh + Mie 単散乱を per-direction で評価し、
 * 焼いた equirect 画像を ImageBasedLighting に渡すと env cubemap → irradiance →
 * prefilter の IBL chain が物理ベースの sky で構築できる。
 */
class FAtmosphere {
public:
    /**
     * CPU で equirect 画像を焼いて RGBA float 配列を返す。
     *
     * @details
     * 出力は w*h*4 個の float で上から下へ並び、v=0 が +Y 天頂、v=1 が -Y 天底
     * (sIBL Archive 規約と一致)。戻り値の TArray は move で呼び出し側に渡される。
     * @param width 焼く equirect 画像の幅 (ピクセル)。
     * @param height 焼く equirect 画像の高さ (ピクセル)。
     * @param params 太陽方向・強度とサンプル数を含む bake パラメータ。
     * @return RGBA float を格納した TArray (w*h*4 要素)。
     */
    static TArray<f32> BakeEquirect(u32 width, u32 height,
                                    const AtmosphereParams& params) noexcept;
};

/**
 * GPU 物理大気 (WickedEngine / Hillaire 2020 流の GPU compute パイプライン)。
 *
 * @details
 * Transmittance LUT (256x64) を compute で焼き、equirect bake compute がそれを使って
 * Rayleigh+Mie+ozone の単散乱 + 等方多重散乱を per-direction で評価して equirect texture
 * (RGBA32F) に書く。ReadTexture で CPU へ読み戻し、ImageBasedLighting::LoadEquirectHdrFromMemory
 * に通せば既存の env cubemap → irradiance → prefilter の IBL chain と背景描画がそのまま動く。
 * CPU 版 FAtmosphere::BakeEquirect の置き換え (GPU で高速 + ozone/multiscatter で物理的に正しい空)。
 * 要 Phase 0 compute コア + DiligentDevice::ReadTexture。Diligent backend 専用。
 */
class FSkyAtmosphere {
public:
    /** compute パイプライン (transmittance / equirect bake) と Transmittance LUT・CB を生成。 */
    TResult<void> Init(IRhiDevice& device) noexcept;

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
                      const AtmosphereParams& params,
                      u32 width, u32 height, TArray<f32>& out) noexcept;

    /**
     * Aerial perspective camera-volume LUT (32^3 froxel) を焼いて返す (WickedEngine 流)。
     *
     * @details
     * 各 froxel = camera→その距離までの大気 in-scatter (.rgb) と opacity (.a=1-平均透過率)。
     * PBR で screen uv + 深度→スライスで trilinear サンプルし col = col*(1-ap.a)+ap.rgb で適用。
     * 3D LUT を物理積分するため滑らか (cubemap サンプルのような «斜めの段» が出ない)。
     * @param inv_view_proj 逆 view-projection (froxel の world ray 復元用)。
     * @param cam_pos カメラ world position (scene 単位)。
     * @param sun_dir 太陽方角 (+Y up)。
     * @param sun_intensity 太陽ピーク輝度。
     * @param max_dist_scene volume がカバーする最大距離 (scene 単位)。
     * @param scene_to_km scene 単位 → 大気 km の換算 (見た目調整。小さいシーンで霞を可視化)。
     * @param cam_alt_km カメラの大気高度 (km、地表 ≈ 0)。
     * @return AP volume (失敗時 nullptr、非所有)。
     */
    IRhiTexture* BuildAerialPerspective(IRhiDevice& device, IRhiCommandList& cl,
                                        const FMat4& inv_view_proj, FVec3 cam_pos,
                                        FVec3 sun_dir, FVec3 sun_intensity,
                                        f32 max_dist_scene, f32 scene_to_km,
                                        f32 cam_alt_km) noexcept;

    /** 直近に焼いた AP volume (BuildAerialPerspective 後に有効、非所有)。 */
    IRhiTexture* ApVolume() const noexcept { return m_ApVol.Get(); }

    /** 全 GPU リソースを解放 (acs_editor_destroy から呼ぶ。UAF 防止)。 */
    void Shutdown() noexcept;

private:
    bool                     m_Ready = false;
    TUniquePtr<IRhiShader>   m_TransCs;
    TUniquePtr<IRhiShader>   m_BakeCs;
    TUniquePtr<IRhiPipeline> m_TransPipe;
    TUniquePtr<IRhiPipeline> m_BakePipe;
    TUniquePtr<IRhiTexture>  m_TransLut;    // 256x64 RGBA16F UAV/SRV
    TUniquePtr<IRhiTexture>  m_Equirect;    // width x height RGBA32F UAV (readback source)
    TUniquePtr<IRhiBuffer>   m_Cb;          // sun dir + intensity
    TUniquePtr<IRhiShader>   m_ApCs;        // aerial perspective froxel CS
    TUniquePtr<IRhiPipeline> m_ApPipe;
    TUniquePtr<IRhiTexture>  m_ApVol;       // 32^3 RGBA16F UAV/SRV (camera-volume AP LUT)
    TUniquePtr<IRhiBuffer>   m_ApCb;        // AP cbuffer (invVP/cam/sun/params)
    u32                      m_EqW = 0, m_EqH = 0;
};

} // namespace acs
