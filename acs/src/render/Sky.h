// SPDX-License-Identifier: Apache-2.0
// 手続き生成スカイ（グラデーション + 太陽）
//
// 用途: 3D シーンの背景に「空」を描く。シーン描画より先に呼ぶ。
//       テクスチャ（キューブマップ）不要、ピクセルシェーダで天頂・地平線・
//       地面の色を補間して描画する。
//
// 使い方:
//   CSky sky;
//   sky.Init(*renderer.Device(), renderer.ColorFormat(), renderer.DepthFormat());
//   sky.PresetDay();
//
//   // 描画フレーム中、シーンの最初に
//   sky.Render(*cl, camera);
//   // ... CStandardShader でメッシュを描く ...
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"

namespace acs {

class CCamera;

/**
 * 手続き生成スカイ (グラデーション + 太陽)。
 *
 * @details
 * テクスチャ (キューブマップ) 不要で、ピクセルシェーダが視線方向から天頂・地平線・
 * 地面の色を補間して背景を描く。シーン描画より先にフルスクリーン三角形で塗り、
 * 深度の書き込み・テストは行わない (背景塗りなので既存深度を維持)。太陽は視線と太陽
 * 方向の角度で半径・ハローを付ける。VS/PS/PSO/定数バッファを単独所有する。
 */
class CSky {
public:
    /** CPU-compiled shader bytecode handed to the render-owner thread. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> vertex;
        TUniquePtr<IRhiShader> pixel;

        /** Aggregate a backend-managed asynchronous compile without waiting. */
        EShaderStatus Status() const noexcept;
    };

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CSky() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~CSky() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CSky(const CSky&)            = delete;

    /** コピー代入も禁止。 */
    CSky& operator=(const CSky&) = delete;

    /**
     * GPU リソース (VS/PS/PSO/定数バッファ) を確保する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param rt_format 描画先カラーターゲットのフォーマット。
     * @param depth_format 深度ターゲットのフォーマット (パイプライン作成用)。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device,
                      EFormat rt_format    = EFormat::B8G8R8A8_UNorm,
                      EFormat depth_format = EFormat::D32_Float) noexcept;

    /**
     * Compile the raw-DX12 HLSL bytecode without touching an RHI device.
     * Other backends retain the regular owner-thread Init path.
     */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /**
     * Submit shader compilation to a supporting RHI backend. Submission and
     * later PSO/resource creation stay on the render-owner thread.
     */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(
        IRhiDevice& device) noexcept;

    /**
     * Install CPU-compiled shaders and create the buffer and PSO.
     * Must be called by the render-owner thread.
     */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        EFormat rt_format = EFormat::B8G8R8A8_UNorm,
        EFormat depth_format = EFormat::D32_Float) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /**
     * 太陽方向を設定する (内部で正規化する)。
     *
     * @details カメラの右手/左手系の前提なし。シェーダ側でも normalize する。
     * @param dir 原点から太陽へ向く方向ベクトル。
     */
    void SetSunDirection(FVec3 dir) noexcept;

    /**
     * 太陽の色を設定する。
     *
     * @param c 太陽の RGB 色。
     */
    void SetSunColor(FVec3 c)       noexcept { m_SunColor = c; }

    /**
     * 太陽の見かけ半径を設定する。
     *
     * @param angular 視線角の cos 値からの差 (0.001 = 鋭い、0.05 = 大きい)。
     */
    void SetSunRadius(f32 angular) noexcept { m_SunRadius  = angular; }

    /**
     * 太陽の周りのハロー (グロー) の広がりを設定する。
     *
     * @param angular ハローの角度的な広がり。
     */
    void SetSunGlow(f32 angular)   noexcept { m_SunGlow    = angular; }

    /**
     * 天頂の色を設定する。
     *
     * @param c 天頂 (真上) 方向の RGB 色。
     */
    void SetZenithColor(FVec3 c)    noexcept { m_Zenith  = c; }

    /**
     * 地平線の色を設定する。
     *
     * @param c 地平線 (dir.y=0) 方向の RGB 色。
     */
    void SetHorizonColor(FVec3 c)   noexcept { m_Horizon = c; }

    /**
     * 地面方向の色を設定する。
     *
     * @param c 地面 (真下) 方向の RGB 色。
     */
    void SetGroundColor(FVec3 c)    noexcept { m_Ground  = c; }

    /**
     * 手続き的な雲の量と濃さを設定する。
     *
     * @param coverage 雲量 (0=快晴、1=全天曇り)。
     * @param density 雲の濃さ/輪郭の鋭さ (1=やわらか、2〜3=もくもく)。
     */
    void SetClouds(f32 coverage, f32 density = 1.6f) noexcept {
        m_CloudCoverage = coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
        m_CloudDensity  = density  < 0.1f ? 0.1f : density;
    }

    /**
     * 雲を描くかどうかを切り替える。
     *
     * @param on true で雲を描画 (既定 ON)。
     */
    void SetCloudsEnabled(bool on)  noexcept { m_bCloudsEnabled = on; }

    /**
     * 雲の基本色を設定する。
     *
     * @param c 雲の RGB 色 (太陽方向で明色に、濃い所は暗色に補間される)。
     */
    void SetCloudColor(FVec3 c)     noexcept { m_CloudColor = c; }

    /**
     * 雲が流れる速さを設定する。
     *
     * @param speed 風速 (0=静止、1=標準)。SetTime と併用してアニメする。
     */
    void SetCloudWind(f32 speed)    noexcept { m_CloudWind = speed; }

    /**
     * 雲アニメ用の時間を設定する (任意。決定論的に制御したいときだけ呼ぶ)。
     *
     * @details 呼ばなくても Render() が内部で時間を進めるので雲は流れる。毎フレーム
     *          経過秒を渡すと、その値が当該フレームで優先される (リプレイ/スクショ向け)。
     * @param seconds 起動からの経過秒など、単調増加する時間値。
     */
    void SetTime(f32 seconds)       noexcept { m_Time = seconds; }

    /** 昼空プリセットを適用する (青空 + 白い太陽)。 */
    void PresetDay()    noexcept;

    /** 夕焼けプリセットを適用する (茜色 + 暖色太陽)。 */
    void PresetSunset() noexcept;

    /** 夜空プリセットを適用する (紺青 + 弱い月光)。 */
    void PresetNight()  noexcept;

    /**
     * 現在の太陽方向を返す (CStandardShader / IBL と整合させたいときに)。
     *
     * @return 正規化済みの太陽方向ベクトル。
     */
    FVec3 SunDirection() const noexcept { return m_SunDir; }

    /**
     * 現在の太陽色を返す。
     *
     * @return 太陽の RGB 色。
     */
    FVec3 SunColor()     const noexcept { return m_SunColor; }

    /**
     * 現在の太陽半径を返す。
     *
     * @return 太陽の見かけ半径 (視線角 cos 値からの差)。
     */
    f32  SunRadius()    const noexcept { return m_SunRadius; }

    /**
     * 現在の太陽ハロー幅を返す。
     *
     * @return 太陽ハローの角度的な広がり。
     */
    f32  SunGlow()      const noexcept { return m_SunGlow; }

    /**
     * 現在の天頂色を返す。
     *
     * @return 天頂方向の RGB 色。
     */
    FVec3 ZenithColor()  const noexcept { return m_Zenith; }

    /**
     * 現在の地平線色を返す。
     *
     * @return 地平線方向の RGB 色。
     */
    FVec3 HorizonColor() const noexcept { return m_Horizon; }

    /**
     * 現在の地面色を返す。
     *
     * @return 地面方向の RGB 色。
     */
    FVec3 GroundColor()  const noexcept { return m_Ground; }

    /**
     * スカイを描画する (フレーム先頭で呼ぶ)。
     *
     * @details 深度バッファは「背景に塗る」想定で書き込み無し・テスト無し。
     * @param cl 描画コマンドを積むコマンドリスト。
     * @param camera 逆 view-projection と視点を取り出すカメラ。
     */
    void Render(IRhiCommandList& cl, const CCamera& camera) noexcept;

private:
    /** フルスクリーン三角形の頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** 空の色を計算するピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** スカイ描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** スカイパラメータを渡す定数バッファ。 */
    TUniquePtr<IRhiBuffer>   m_Cb;

    /** 太陽方向 (正規化済み)。 */
    FVec3 m_SunDir    = FVec3{0.5f, 0.8f, 0.3f};

    /** 太陽の RGB 色。 */
    FVec3 m_SunColor  = FVec3{1.0f, 0.95f, 0.85f};

    /** 太陽の見かけ半径 (視線角 cos 値からの差)。 */
    f32  m_SunRadius = 0.0006f;

    /** 太陽ハローの角度的な広がり。 */
    f32  m_SunGlow   = 0.04f;

    /** 天頂方向の RGB 色。 */
    FVec3 m_Zenith     = FVec3{0.18f, 0.40f, 0.78f};

    /** 地平線方向の RGB 色。 */
    FVec3 m_Horizon    = FVec3{0.70f, 0.80f, 0.95f};

    /** 地面方向の RGB 色。 */
    FVec3 m_Ground     = FVec3{0.20f, 0.18f, 0.16f};

    /** 雲を描画するか (既定 ON)。 */
    bool m_bCloudsEnabled = true;

    /** 雲量 (0=快晴、1=全天曇り)。 */
    f32  m_CloudCoverage = 0.50f;

    /** 雲の濃さ/輪郭の鋭さ。 */
    f32  m_CloudDensity  = 1.6f;

    /** 雲が流れる速さ。 */
    f32  m_CloudWind     = 1.0f;

    /** 雲アニメ用の時間 (SetTime で更新)。 */
    f32  m_Time          = 0.0f;

    /** 雲の基本 RGB 色。 */
    FVec3 m_CloudColor   = FVec3{1.0f, 1.0f, 1.0f};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FSky = CSky;


/**
 * GPU レイマーチ volumetric clouds (WickedEngine / Nubis 流)。
 *
 * @details
 * compute シェーダで全画面の視線ごとに雲スラブをレイマーチし、3D 手続きノイズ (Worley FBM) の
 * 密度を coverage/height-gradient で remap、太陽方向へ light-march して Beer 透過率を求め、
 * dual-lobe Henyey-Greenstein 位相 + powder 項でエネルギー保存散乱を積分する。出力 (straight 散乱色
 * + alpha) を hdrRt の «空» の上に合成する。ray march は half-res、雲自身の代表深度を使う
 * bilateral spatial reconstruction と camera/wind reprojection temporal accumulation で full-res に
 * 復元する。CSky の 2D-FBM 雲より遥かにディテール/立体感が高い。
 * 要 Phase 0 compute コア (RWTexture2D UAV)。full-res color/depth も一つの
 * 8x8 compute pass で別 format UAV へ同時に再構成し、重複 read と MRT overhead を避ける。
 */
/**
 * World-space altitude band used by CVolumetricClouds.
 *
 * The cloud density field must never be translated with the camera.  Keeping
 * these heights in world space makes translation, orbit and temporal
 * reprojection observe the same density field.
 */
struct FVolumetricCloudLayer {
    f32 base_height = 1500.0f;
    f32 top_height = 4000.0f;
    f32 horizontal_noise_scale = 0.035f;
};

/** Ray interval through a horizontal world-space cloud layer. */
struct FVolumetricCloudRayInterval {
    f32 enter = 0.0f;
    f32 exit = 0.0f;
    bool hit = false;
};

/** Local planet radius used by the curved world-space cloud shell. */
inline constexpr f32 kVolumetricCloudPlanetRadius = 6360000.0f;

/**
 * World-origin grid used by the local curved-shell patch.
 *
 * The shell origin stays fixed through the centre of each cell and is eased
 * across a transition band near cell boundaries. Density/weather coordinates
 * remain absolute world coordinates; only the numerically local planet
 * tangent patch is rebased.
 */
inline constexpr f32 kVolumetricCloudOriginRebaseGrid = 64.0f;
inline constexpr f32 kVolumetricCloudMaxDistance = 250000.0f;

/**
 * Internal trace divisor used by the Ultra output-quality policy.
 *
 * Ultra still resolves and composites at the complete viewport resolution.
 * Every reduced texel is freshly marched each frame, and a sixteen-phase 4x4
 * subpixel schedule maps those rays onto exact full-resolution coordinates.
 * World/depth reprojection retains the other fifteen phases, so the steady
 * image recovers native detail without increasing the quarter-size workload.
 */
inline constexpr u32 kVolumetricCloudUltraTraceDivisor = 4u;
inline constexpr u32 kVolumetricCloudMaxViewMarchSamples = 192u;
inline constexpr u32 kVolumetricCloudMaxLightMarchSamples = 8u;

/** Sanitized current-trace dimensions selected for a full-resolution output. */
struct FVolumetricCloudTraceResolution {
    u32 width = 1u;
    u32 height = 1u;
    f32 quality_multiplier = 1.0f;
    f32 effective_dimension_scale = 0.25f;
};

/**
 * Resolve the authored CloudRenderScale and the internal Ultra trace policy.
 *
 * CloudRenderScale is a monotonic quality multiplier over the policy's base
 * quarter-dimension trace: authored 1.0 uses quarter dimensions, 0.75 uses
 * 0.1875 dimensions, and 0.5 or below uses the 0.125 lower bound. The resolved
 * output and temporal history remain full resolution.
 */
FVolumetricCloudTraceResolution ResolveVolumetricCloudTraceResolution(
    u32 full_width, u32 full_height, f32 requested_render_scale) noexcept;

/**
 * Inputs used to account for the exact compute work submitted by one cloud
 * frame. These booleans describe dispatches, not authoring quality levels.
 */
struct FVolumetricCloudFrameWorkloadPlan {
    u32 trace_width = 0u;
    u32 trace_height = 0u;
    u32 output_width = 0u;
    u32 output_height = 0u;
    bool bake_shape_noise = false;
    bool bake_weather = false;
    bool bake_detail_noise = false;
    bool bake_curl_noise = false;
    bool rebuild_shadow_cache = false;
};

enum class EVolumetricCloudFrameSkipReason : u32 {
    None = 0u,
    ResourcesNotReady = 1u,
    InvalidCamera = 2u,
    InvalidProjection = 3u,
};

/**
 * Allocation-free diagnostic for the most recent RenderCompute attempt.
 *
 * Logical invocations exclude workgroup padding; launched threads include it.
 * maximum_*_samples are conservative shader-loop ceilings rather than measured
 * samples because empty-space skipping and transmittance exits are data
 * dependent. GPU timestamps remain authoritative for elapsed cost.
 */
struct FVolumetricCloudFrameWorkload {
    u64 submission_index = 0u;
    u32 trace_width = 0u;
    u32 trace_height = 0u;
    u32 output_width = 0u;
    u32 output_height = 0u;

    u32 steady_dispatches = 0u;
    u32 one_time_bake_dispatches = 0u;
    u32 shadow_cache_dispatches = 0u;
    u32 total_compute_dispatches = 0u;
    u32 composite_draws = 0u;

    u64 trace_logical_invocations = 0u;
    u64 trace_launched_threads = 0u;
    u64 resolve_logical_invocations = 0u;
    u64 resolve_launched_threads = 0u;
    u64 one_time_bake_logical_invocations = 0u;
    u64 one_time_bake_launched_threads = 0u;
    u64 shadow_cache_logical_invocations = 0u;
    u64 shadow_cache_launched_threads = 0u;
    u64 total_logical_invocations = 0u;
    u64 total_launched_threads = 0u;
    u64 maximum_view_samples = 0u;
    u64 maximum_light_samples = 0u;

    EVolumetricCloudFrameSkipReason skip_reason =
        EVolumetricCloudFrameSkipReason::None;
    bool attempted = false;
    bool submitted = false;
    bool history_was_available = false;
    bool history_reused = false;
    bool history_invalidated = false;
    bool temporal_super_resolution = false;
};

/**
 * Deterministically account for a cloud frame without recording GPU work.
 *
 * All additions and products saturate at u64 max so malformed diagnostic input
 * cannot wrap into a deceptively small workload.
 */
FVolumetricCloudFrameWorkload PlanVolumetricCloudFrameWorkload(
    const FVolumetricCloudFrameWorkloadPlan& plan) noexcept;

/**
 * Resolve centered analytic coverage for the planet/cloud horizon.
 *
 * elevation_delta_x/y are the signed elevation differences to exact adjacent
 * screen-space rays.  The footprint is centered on the physical tangent so a
 * sloped horizon crosses pixels continuously instead of snapping to a
 * one-sided row.  Non-finite inputs fail closed to zero coverage.
 */
f32 ResolveVolumetricCloudHorizonCoverage(
    f32 signed_elevation, f32 cutoff,
    f32 elevation_delta_x, f32 elevation_delta_y) noexcept;

/**
 * Per-frame camera/planet terms shared by every cloud trace/resolve pixel.
 *
 * local_up is the normalized camera vector from the rebased planet centre.
 * ground_cutoff is the signed ray-elevation tangent of the physical ground
 * sphere. Values below -1 disable the cutoff when the camera is in/above the
 * authored cloud layer or when hostile input cannot be represented safely.
 */
struct FVolumetricCloudGroundHorizon {
    FVec3 local_up{0.0f, 1.0f, 0.0f};
    f32 ground_cutoff = -2.0f;
};

/**
 * Hoist camera-invariant ground-horizon geometry out of per-pixel shaders.
 *
 * The equations deliberately mirror cloudAltitude/groundCutoff in kCloudCS.
 * Ordinary finite inputs therefore preserve the analytic two-axis horizon
 * coverage while avoiding identical divisions, normalization and square root
 * work in every trace and full-resolution resolve invocation.
 */
FVolumetricCloudGroundHorizon ResolveVolumetricCloudGroundHorizon(
    FVec3 camera_position, const FVolumetricCloudLayer& layer,
    FVec3 world_origin) noexcept;

/**
 * Density-domain terms that are constant for a complete cloud frame.
 *
 * Keeping these values in CloudCB prevents every view and light-cone sample
 * from rebuilding the identical world wind, shape frequency, and layer-height
 * reciprocal. The density coordinates remain absolute world-space values.
 */
struct FVolumetricCloudDensityFrameTerms {
    FVec2 wind_world{};
    f32 shape_scale = 0.00012f;
    f32 inverse_layer_height = 1.0f;
};

FVolumetricCloudDensityFrameTerms ResolveVolumetricCloudDensityFrameTerms(
    const FVolumetricCloudLayer& layer, f32 wind_offset) noexcept;

/**
 * Normalized sun direction and its continuous Duff/Frisvad tangent basis.
 *
 * The basis is frame-invariant and is shared by every cloud view/light probe.
 */
struct FVolumetricCloudLightBasis {
    FVec3 direction{0.0f, 1.0f, 0.0f};
    FVec3 tangent{1.0f, 0.0f, 0.0f};
    FVec3 bitangent{0.0f, 0.0f, -1.0f};
};

FVolumetricCloudLightBasis ResolveVolumetricCloudLightBasis(
    FVec3 sun_direction) noexcept;

/**
 * Experimental hybrid shallow sun optical-depth cache.
 *
 * The current two-volume implementation costs more GPU time than the exact
 * far-light tail on the measured desktop path, so keep it compiled for
 * further iteration but do not allocate or dispatch it by default.
 */
inline constexpr bool kVolumetricCloudShadowCacheEnabled = false;

/** Quality-preserving shallow sun optical-depth cache dimensions. */
inline constexpr u32 kVolumetricCloudShadowCacheWidth = 96u;
inline constexpr u32 kVolumetricCloudShadowCacheHeight = 32u;
inline constexpr u32 kVolumetricCloudShadowCacheDepth = 96u;
inline constexpr f32 kVolumetricCloudShadowCacheExtent = 48000.0f;
inline constexpr f32 kVolumetricCloudShadowCacheCellSize =
    kVolumetricCloudShadowCacheExtent /
    static_cast<f32>(kVolumetricCloudShadowCacheWidth);
inline constexpr f32 kVolumetricCloudShadowCacheSafeRadius = 8000.0f;

/** Stable material-space footprint used by the cloud sun-depth cache. */
struct FVolumetricCloudShadowCacheMapping {
    FVec2 min_material_xz{};
    FVec2 center_material_xz{};
};

/** Convert the scalar cloud advection distance to its shared XZ direction. */
FVec2 VolumetricCloudWindOffsetXZ(f32 wind_offset) noexcept;

/** Absolute world XZ with the shared cloud advection removed. */
FVec2 VolumetricCloudMaterialXZ(FVec3 world_position,
                                f32 wind_offset) noexcept;

/** Snap a cache footprint to the material-space voxel lattice. */
FVolumetricCloudShadowCacheMapping CenterVolumetricCloudShadowCache(
    FVec2 material_position) noexcept;

/**
 * Compute the soft-snapped XZ world origin used by the curved cloud shell.
 *
 * The returned Y is zero so authored cloud heights retain their world-Y
 * meaning. The cell centre is stationary during ordinary editor orbits, while
 * a C1-continuous transition near cell boundaries avoids a full-cell shell
 * jump. The density field itself is never translated with the camera.
 */
FVec3 RebaseVolumetricCloudWorldOrigin(
    FVec3 camera_position,
    f32 grid_size = kVolumetricCloudOriginRebaseGrid) noexcept;

/**
 * Stable world-distance march parameters for a cloud ray.
 *
 * Uniformly splitting the layer by height makes every pixel sample the same
 * horizontal planes.  In perspective that correlation appears as rays
 * converging on the view zenith.  A world-distance step keeps the sampling
 * frequency independent of view angle; empty space may use coarse_step while
 * occupied density uses fine_step.
 */
struct FVolumetricCloudMarchPlan {
    f32 enter = 0.0f;
    f32 exit = 0.0f;
    f32 fine_step = 1.0f;
    f32 coarse_step = 4.0f;
    f32 visibility = 0.0f;
    u32 max_samples = kVolumetricCloudMaxViewMarchSamples;
    bool hit = false;
};

/**
 * Intersect a world-space ray with a horizontal cloud altitude band.
 * This CPU companion mirrors the shader interval calculation and is useful for
 * camera/world-anchoring validation.
 */
FVolumetricCloudRayInterval IntersectVolumetricCloudLayer(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& layer) noexcept;

/**
 * Intersect the nearest continuous segment of a curved cloud shell.
 *
 * The planet centre is
 * (world_origin.x, world_origin.y-planet_radius, world_origin.z).  A discrete
 * rebased origin keeps the local tangent patch numerically stable during long
 * XZ travel without making the density field camera-relative.
 */
FVolumetricCloudRayInterval IntersectVolumetricCloudShell(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& layer,
    f32 planet_radius = kVolumetricCloudPlanetRadius,
    FVec3 world_origin = FVec3{}) noexcept;

/**
 * Build the bounded, angle-stable ray-march plan mirrored by the cloud shader.
 */
FVolumetricCloudMarchPlan PlanVolumetricCloudRayMarch(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& layer,
    f32 max_distance = kVolumetricCloudMaxDistance,
    FVec3 world_origin = FVec3{}) noexcept;

/**
 * Return true when lighting changes make accumulated cloud color stale.
 *
 * Direction is expected to be normalized, matching RenderCompute's stored
 * frame signature.
 */
bool VolumetricCloudLightingChanged(
    FVec3 previous_sun_direction, FVec3 previous_sun_color,
    FVec3 previous_sky_color, FVec3 sun_direction,
    FVec3 sun_color, FVec3 sky_color) noexcept;

/**
 * Detect a discontinuous camera cut without treating ordinary world-space
 * translation as a cut.
 *
 * Comparing view-projection matrix elements directly also compares their
 * translation terms. In a metre-scale editor that invalidates temporal cloud
 * history during normal fly/pan motion and repeatedly exposes the cold 4x4
 * reconstruction pattern. This comparison instead measures representative
 * view-ray directions and reserves the distance test for a real teleport.
 */
bool VolumetricCloudViewCutDetected(
    const FMat4& previous_inv_view_proj, FVec3 previous_camera_position,
    const FMat4& current_inv_view_proj,
    FVec3 current_camera_position) noexcept;

class CVolumetricClouds {
public:
    /** CPU-compiled shader bytecode handed to the render-owner thread. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> cloud;
        TUniquePtr<IRhiShader> noise;
        TUniquePtr<IRhiShader> weather;
        TUniquePtr<IRhiShader> detail;
        TUniquePtr<IRhiShader> curl;
        TUniquePtr<IRhiShader> composite_vertex;
        TUniquePtr<IRhiShader> composite_pixel;
        TUniquePtr<IRhiShader> composite_atmosphere_pixel;
        TUniquePtr<IRhiShader> resolve;
        TUniquePtr<IRhiShader> shadow;
        TUniquePtr<IRhiShader> shadow_finalize;

        /** Aggregate all submitted shader jobs without waiting. */
        EShaderStatus Status() const noexcept;
    };

    /** compute (雲レイマーチ) + composite (全画面 alpha blend) パイプラインを生成。 */
    TResult<void> Init(IRhiDevice& device, EFormat hdr_format) noexcept;

    /** Compile raw-DX12 HLSL without touching an RHI device. */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /** Submit all cloud shaders to a backend-managed compiler pool. */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(
        IRhiDevice& device) noexcept;

    /**
     * Create owner-thread resources from CPU-compiled bytecode and publish the
     * complete mandatory cloud resource set atomically.
     */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        EFormat hdr_format) noexcept;

    /** 初期化済みか。 */
    bool Ready() const noexcept { return m_Ready; }

    /**
     * Scaled ray-march output and full-resolution reconstruction historyを確保/再確保。
     * render_scale は Ultra internal quarter-dimension trace に対する
     * 0.5..1.0 の品質倍率として扱い、resolved output は常に full-resolution。
     */
    bool EnsureSize(IRhiDevice& device, u32 scW, u32 scH,
                    f32 render_scale = 0.5f) noexcept;

    /** Set the fixed world-space cloud altitude band and invalidate history. */
    void SetLayer(const FVolumetricCloudLayer& layer) noexcept;

    /** Current sanitized world-space cloud altitude band. */
    const FVolumetricCloudLayer& Layer() const noexcept { return m_Layer; }

    /**
     * Keep allocated cloud targets but reject the previous view's temporal
     * reconstruction history on the next render.
     */
    void InvalidateHistory() noexcept { m_HistoryValid = false; }

    /** Logical sun-depth rebuilds; the raw/finalize dispatch pair counts once. */
    u64 ShadowCacheDispatchCount() const noexcept {
        return m_ShadowCacheDispatchCount;
    }

    /** Whether the optional cache resources were created successfully. */
    bool ShadowCacheAvailable() const noexcept {
        return m_ShadowCacheAvailable;
    }

    /** Whether the current material-space cache key has valid GPU contents. */
    bool ShadowCacheValid() const noexcept { return m_ShadowCacheValid; }

    /** Exact submitted-work accounting for the latest compute/composite frame. */
    const FVolumetricCloudFrameWorkload& LastFrameWorkload() const noexcept {
        return m_LastFrameWorkload;
    }

    /** Full-resolution resolved cloud distance/confidence for later fog passes. */
    IRhiTexture* ResolvedDepth() const noexcept {
        return m_HistoryValid ? m_HistoryDepth[m_ResolvedIndex].Get() : nullptr;
    }

    /**
     * 雲を compute でレイマーチして内部 UAV テクスチャへ書く (render pass の «外» で呼ぶ)。
     * Ultra は毎 frame quarter-dimension の全 texel を更新し、それぞれを 4x4 block
     * 内の exact full-resolution subpixel へ 16 phase で割り当てる。camera motion や
     * 履歴無効化でも native/full seed に戻らず、未更新 subpixel は world/depth
     * reprojection、初回/disocclusion は spatial fallback で解決する。
     */
    void RenderCompute(IRhiCommandList& cl, const FMat4& inv_view_proj, FVec3 cam_pos,
                       FVec3 sun_dir, FVec3 sun_color, FVec3 sky_color,
                       f32 coverage, f32 density, f32 wind, f32 time) noexcept;

    /**
     * 雲 (straight 散乱色+alpha) を現在の RT へ alpha blend する。
     * 解決済み cloud ray distance と scene_depth から復元した scene ray distance を比較し、
     * 手前にある方だけを表示する。カメラが雲層内/上空にいる場合も前景雲を失わない。
     * atmosphere_volume と atmosphere_transmittance がある場合は、可視 cloud の
     * 代表距離までの RGB transmittance と premultiplied atmospheric in-scatter を
     * 適用してから背景へ alpha blend する。
     * local fog は ResolvedDepth() を使う後段 pass で同じ cloud 距離へ終端する。
     * depth は SRV として読むので、この render pass の DSV には同時 bind しないこと。
     */
    void Composite(IRhiCommandList& cl, IRhiTexture& scene_depth,
                   u32 scW, u32 scH,
                   IRhiTexture* atmosphere_volume = nullptr,
                   IRhiTexture* atmosphere_transmittance = nullptr,
                   f32 atmosphere_max_distance = 1.0f) noexcept;

    /** 全 GPU リソースを解放 (acs_editor_destroy から呼ぶ。UAF 防止)。 */
    void Shutdown() noexcept;

private:
    TResult<void> InitCandidateWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        EFormat hdr_format) noexcept;

    bool                     m_Ready = false;
    bool                     m_NoiseBaked = false;       // Phase 4.5: shape noise を焼いたか
    EFormat                  m_HdrFormat = EFormat::R16G16B16A16_Float;
    TUniquePtr<IRhiShader>   m_NoiseCs;                  // Phase 4.5: Perlin-Worley 生成 compute
    TUniquePtr<IRhiPipeline> m_NoisePipe;                // compute (noise gen)
    TUniquePtr<IRhiTexture>  m_ShapeTex;                 // 128^3 RG16F Perlin-Worley/low-frequency Worley
    bool                     m_WeatherBaked = false;
    TUniquePtr<IRhiShader>   m_WeatherCs;
    TUniquePtr<IRhiPipeline> m_WeatherPipe;
    TUniquePtr<IRhiTexture>  m_WeatherTex;               // 512^2 coverage/type/precipitation/warp
    bool                     m_DetailBaked = false;
    TUniquePtr<IRhiShader>   m_DetailCs;
    TUniquePtr<IRhiPipeline> m_DetailPipe;
    TUniquePtr<IRhiTexture>  m_DetailTex;                // 64^3 RG16F independent Worley edge erosion
    bool                     m_CurlBaked = false;
    TUniquePtr<IRhiShader>   m_CurlCs;
    TUniquePtr<IRhiPipeline> m_CurlPipe;
    TUniquePtr<IRhiTexture>  m_CurlTex;                  // 128^2 independent world-space curl warp
    TUniquePtr<IRhiShader>   m_CloudCs;
    TUniquePtr<IRhiPipeline> m_CloudPipe;     // compute
    TUniquePtr<IRhiShader>   m_ShadowCs;      // raw shallow sun optical depth
    TUniquePtr<IRhiPipeline> m_ShadowPipe;
    TUniquePtr<IRhiShader>   m_ShadowFinalizeCs; // bake spatial confidence
    TUniquePtr<IRhiPipeline> m_ShadowFinalizePipe;
    TUniquePtr<IRhiTexture>  m_ShadowRawTex;  // 96x32x96 mean/pattern error tau
    TUniquePtr<IRhiTexture>  m_ShadowTex;     // 96x32x96 mean/max error tau
    TUniquePtr<IRhiShader>   m_CompVs, m_CompPs;
    TUniquePtr<IRhiPipeline> m_CompPipe;      // graphics (alpha blend)
    TUniquePtr<IRhiShader>   m_CompAtmosPs;
    TUniquePtr<IRhiPipeline> m_CompAtmosPipe; // cloud-distance terminated physical AP (RGB L/T)
    TUniquePtr<IRhiBuffer>   m_CompAtmosCb;
    TUniquePtr<IRhiShader>   m_ResolveCs;      // 深度対応の color/depth 空間・時間再構成
    TUniquePtr<IRhiPipeline> m_ResolvePipe;    // 全解像度 color/depth compute UAV 解決
    TUniquePtr<IRhiBuffer>   m_Cb;
    TUniquePtr<IRhiTexture>  m_CloudTex;       // scaled ray-march RGBA16F の非乗算カラー + アルファ
    TUniquePtr<IRhiTexture>  m_CloudDepth;     // scaled ray-march RG32F (代表距離、信頼度)
    TUniquePtr<IRhiTexture>  m_HistoryColor[2];// 全解像度 RGBA16F の時間履歴 ping-pong
    TUniquePtr<IRhiTexture>  m_HistoryDepth[2];// 全解像度 RG32F の雲距離・信頼度 ping-pong
    FMat4                    m_PrevViewProj = FMat4::Identity();
    FMat4                    m_PrevInvViewProj = FMat4::Identity();
    FVec3                    m_PrevCamPos{};
    FVec3                    m_WorldOrigin{};
    FVec3                    m_PrevSunDir{};
    FVec3                    m_PrevSunColor{};
    FVec3                    m_PrevSkyColor{};
    f32                      m_PrevWindOffset = 0.0f;
    f32                      m_PrevWindSpeed = 0.0f;
    f32                      m_PrevCoverage = -1.0f;
    f32                      m_PrevDensity = -1.0f;
    f32                      m_PrevTime = -1.0f;
    FVec2                    m_ShadowGridMinQ{};
    FVec2                    m_ShadowGridCenterQ{};
    FVec2                    m_ShadowCurvatureAnchor{};
    FVec3                    m_ShadowSunDir{};
    FVolumetricCloudLayer    m_ShadowLayer{};
    f32                      m_ShadowCoverage = -1.0f;
    u64                      m_ShadowCacheDispatchCount = 0;
    bool                     m_ShadowGridInitialized = false;
    bool                     m_ShadowCacheAvailable = false;
    bool                     m_ShadowCacheValid = false;
    FVolumetricCloudLayer    m_Layer{};
    u32                      m_FrameIndex = 0;
    u32                      m_TemporalPhase = 0;
    u32                      m_ResolvedIndex = 0;
    bool                     m_HistoryValid = false;
    u32                      m_W = 0, m_H = 0;         // scaled ray-march の寸法
    u32                      m_FullW = 0, m_FullH = 0; // 再構成先の寸法
    u64                      m_WorkloadSubmissionIndex = 0u;
    FVolumetricCloudFrameWorkload m_LastFrameWorkload{};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FVolumetricClouds = CVolumetricClouds;


} // namespace acs
