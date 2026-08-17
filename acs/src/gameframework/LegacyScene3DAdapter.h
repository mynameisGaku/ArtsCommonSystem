// SPDX-License-Identifier: Apache-2.0
// Reversible AScene host for legacy ACS3D editor documents.
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Scene.h"
#include "gameframework/LightCollector3D.h"
#include "render/ShadowMap.h"
#include "render/Ibl.h"
#include "render/Atmosphere.h"
#include "gameframework/SceneNodeGraph.h"
#include "gameframework/Scene3DSerialize.h"
#include "math/Camera.h"
#include "math/Collision3D.h"   // FRay3 / FRayHit3 (RaycastWater と mesh 交差)
#include "render/Blit.h"
#include "render/PbrShader.h"
#include "render/PostProcess.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/SubsurfaceScattering.h"
#include "render/WaterSurface3D.h"
#include "threading/Thread.h"

#include <atomic>

namespace acs::game {

class IAssetPackReader;
class AMeshComponent3D;
class AWaterSurface3DComponent;

/**
 * Exact gameplay hit returned by ALegacyScene3DAdapter::RaycastWater.
 *
 * @details Point and normal are expressed in world space. Distance is the
 * parameter on the caller's ray (`origin + direction * distance`), so callers
 * may use normalized or deliberately scaled directions without a hidden
 * conversion.
 */
struct FWaterRaycastHit {
    FNodeId Node{};
    FVec3 Point{};
    FVec3 Normal{0.0f, 1.0f, 0.0f};
    f32 Distance = 0.0f;

    bool IsValid() const noexcept { return Node.IsValid(); }
};

/**
 * Active projection state for the canonical scene runtime.
 *
 * @details ACS3D CAM3D may author the initial projection. An editor 2D view may
 * still select Orthographic without converting the root graph or the dedicated
 * 2D renderer/physics subsystems.
 */
enum class ESceneProjectionMode : u8 {
    Perspective = 0,
    Orthographic = 1,
};

/**
 * 旧 `ACS3D v2` document を AScene へ接続する adapter。
 *
 * @details scene graph は editor runtime と共有する AScene 所有の ANode/FTransform3D graph。
 * 永続 scene asset type を増やさず adapter として実装する。package は 1 つの
 * `main.acscene` bootstrap entry を公開し、検証済み header から旧 .acscene/.acs3d reader を
 * 選ぶ。Sprite batching、Canvas/UI、2D physics は専用 runtime path に残す。
 */
/**
 * 距離で霞ませる霧の設定。
 *
 * @details
 * 濃さ 1 つで画の締まりが大きく変わる。既定はうっすら掛かる程度。
 * 高さの効き方は「基準の高さより上ほど薄い」。
 */
/**
 * 空に浮かべる雲の設定。
 *
 * @details
 * `Coverage` が 0 なら出ない。厚みは `BaseAltitude` と `TopAltitude` の差で、
 * 世界の単位で指定する。
 */
struct FScene3DClouds {
    /** 空をどれだけ覆うか (0 で出ない、1 で一面)。 */
    f32 Coverage = 0.0f;

    /** 濃さ。大きいほど光を通さず、輪郭がはっきりする (エディタと同じ既定)。 */
    f32 Density = 1.6f;

    /** 流れる速さ (エディタと同じ既定)。 */
    f32 Wind = 1.0f;

    /** 雲の底の高さ。 */
    f32 BaseAltitude = 1500.0f;

    /** 雲の上端の高さ。 */
    f32 TopAltitude = 4000.0f;

    /**
     * 形の細かさ。
     *
     * @details
     * **既定はエンジンの `FVolumetricCloudLayer` と同じ値。** 大きくすると細かくなりすぎて、
     * 雲の塊にならず «何も出ていない» ように見える。
     */
    f32 NoiseScale = 0.035f;

    /**
     * 描く大きさの割合。
     *
     * @details 1 未満にすると内部を小さく描いて引き伸ばす。重いので下げる余地を残してある。
     * 既定はエディタの «高品質» と同じ。
     */
    f32 RenderScale = 0.75f;
};

struct FScene3DFog {
    /** 霧の色 (線形)。遠くのものがこの色へ寄っていく。 */
    FVec3 Color{0.08f, 0.11f, 0.16f};

    /** 濃さ。0 で切れる。大きいほど近くから霞む。 */
    f32 Density = 0.0035f;

    /** 高さによる減り方。大きいほど上空で薄くなる。 */
    f32 HeightFalloff = 0.12f;

    /**
     * 高さの基準。
     *
     * @details 既定 (`FLT_MAX`) のときは、シーンの位置から自動で決める。
     */
    f32 HeightBase = FLT_MAX;
};

class ALegacyScene3DAdapter : public AScene {
public:
    ALegacyScene3DAdapter() noexcept = default;
    ~ALegacyScene3DAdapter() noexcept override;

    ALegacyScene3DAdapter(const ALegacyScene3DAdapter&) = delete;
    ALegacyScene3DAdapter& operator=(const ALegacyScene3DAdapter&) = delete;

    /** Load a loose legacy ACS3D document and all of its mesh/material dependencies. */
    FScene3DLoadResult LoadFile(const char* path = "main.acscene") noexcept;

    /** Load an in-memory ACS3D document transactionally (tests/tools/hot reload). */
    FScene3DLoadResult LoadText(const char* text, u32 size) noexcept;

    /** Load a legacy ACS3D document and all dependencies from one mounted asset pack. */
    FScene3DLoadResult LoadAssetPack(
        IAssetPackReader& pack,
        const char* virtual_path = "main.acscene") noexcept;

    /** Last checked document/dependency result. */
    const FScene3DLoadResult& LoadResult() const noexcept { return m_LoadResult; }

    /** Select the active camera projection without changing the scene document. */
    void SetProjectionMode(ESceneProjectionMode mode) noexcept { m_Projection = mode; }

    /** Active camera projection. */
    ESceneProjectionMode ProjectionMode() const noexcept { return m_Projection; }

    /**
     * 雲の設定を触る。
     *
     * @details
     * `Coverage` を 0 にすると出ない (既定)。出すと、太陽の側が明るく縁が光る本物の雲になる。
     * 空を焼いた cubemap には雲が入らないので、**雲は環境光には効かない** (影も落とさない)。
     * @return 雲の設定 (次のフレームから効く)。
     */
    FScene3DClouds& Clouds() noexcept { return m_CloudParams; }

    /**
     * 雲の設定を読む。
     *
     * @return 雲の設定。
     */
    const FScene3DClouds& Clouds() const noexcept { return m_CloudParams; }

    /**
     * 大気の設定を触る (地面の色、散乱の細かさ)。
     *
     * @details
     * **太陽の向きと強さは毎フレーム上書きされる** (シーンの光から取る)。触れるのは残り。
     *
     * いちばん効くのは `ground_albedo`。大気は「地面で跳ね返った光」も計算するので、
     * ここが実際の地面と違う色だと、**地平線から下が別の場所の色になる**。
     * 草地なら緑寄り、砂漠なら黄寄り、雪原なら白に近く。
     * @return 大気の設定 (次に焼き直すときから効く)。
     */
    FAtmosphereParams& Atmosphere() noexcept { return m_AtmosphereParams; }

    /**
     * 大気の設定を読む。
     *
     * @return 大気の設定。
     */
    const FAtmosphereParams& Atmosphere() const noexcept { return m_AtmosphereParams; }

    /**
     * 距離で霞ませる霧の設定。
     *
     * @details
     * **見え方への影響が大きい割に、これまで場面から触れなかった。** 濃さ 1 つで画の締まりが
     * 決まる。遠景を隠したいなら濃く、物の質感を見せたいなら薄く。
     *
     * `Density` を 0 にすると霧が切れる。
     * @return 霧の設定 (次のフレームから効く)。
     */
    FScene3DFog& Fog() noexcept { return m_Fog; }

    /**
     * 霧の設定を読む。
     *
     * @return 霧の設定。
     */
    const FScene3DFog& Fog() const noexcept { return m_Fog; }

    /**
     * 仕上げ (露出・bloom・tonemap・vignette) の設定を触る。
     *
     * @details
     * 既定では自動露出が入っている。物理ベースの明るさをそのまま出すと画面が飛ぶため。
     * `exposure` はそこへの手動補正 (EV) として働く。
     * @return 仕上げの設定 (次のフレームから効く)。
     */
    FPostProcessParams& PostParams() noexcept { return m_PostParams; }

    /**
     * 仕上げの設定を読む。
     *
     * @return 仕上げの設定。
     */
    const FPostProcessParams& PostParams() const noexcept { return m_PostParams; }

    /**
     * 空の設定を触る (雲・色・太陽の見た目・時刻)。
     *
     * @details
     * **太陽の向きと色は毎フレーム上書きされる。** シーンに平行光源があればそれに、
     * 無ければ既定の太陽に合わせる。空だけ別の方角に太陽を描くと、物の陰りと
     * 食い違って «何かおかしい» 画になるため。
     *
     * それ以外 (雲の量・風・地平の色・時刻) は好きに設定してよい。
     * @return 空。
     */
    CSky& Sky() noexcept { return m_Sky; }

    /**
     * 空の設定を読む。
     *
     * @return 空。
     */
    const CSky& Sky() const noexcept { return m_Sky; }

    /** Camera used for standalone preview/gameplay. */
    CCamera& Camera() noexcept { return m_Camera; }

    /** Read-only standalone camera. */
    const CCamera& Camera() const noexcept { return m_Camera; }

    /** Deterministically selected authored camera, or null for frame-scene fallback. */
    const FScene3DCameraState* AuthoredCamera() const noexcept {
        return m_UseAuthoredCamera ? &m_AuthoredCamera : nullptr;
    }

    /** Number of graph-owned authored camera components. */
    u32 CameraCount() const noexcept;

    /**
     * Switch by canonical stable identity. Unknown or effectively disabled
     * identities fail without changing the active camera.
     */
    bool SetActiveCamera(const char* stable_id) noexcept;

    /**
     * Switch by serialized N3D node id. Unknown or effectively disabled nodes
     * fail without changing the active camera.
     */
    bool SetActiveCamera(i32 node_id) noexcept;

    /** Return an explicit runtime camera cut to authored automatic selection. */
    bool ClearActiveCameraOverride() noexcept;

    /** Descriptive alias for returning to deterministic authored selection. */
    bool UseAutomaticCameraSelection() noexcept {
        return ClearActiveCameraOverride();
    }

    /** Refresh the active camera from its node's current world transform. */
    bool RefreshActiveCamera() noexcept {
        return RefreshAuthoredCameraPose();
    }

    /** Recompute a useful camera target/distance from all renderable nodes. */
    void FrameScene() noexcept;

    /**
     * Raycast authorable water without consuming or capturing platform input.
     *
     * @details Plane primitives use an exact bounded-plane test and custom
     * meshes use their authored triangles. A nearer visible opaque mesh rejects
     * a water hit, preventing interactions through foreground geometry.
     */
    bool RaycastWater(
        const FRay3& ray,
        FWaterRaycastHit& out_hit,
        f32 max_distance = 3.4028235e38f) const noexcept;

    /** Add an impact to one exact water surface. */
    bool AddWaterDisturbance(
        FNodeId surface,
        FVec3 world_point,
        f32 radius = 0.18f,
        f32 strength = 0.22f) noexcept;

    /** Add a directional wake to one exact water surface. */
    bool AddWaterWake(
        FNodeId surface,
        FVec3 world_point,
        FVec3 world_velocity,
        f32 radius = 0.20f,
        f32 strength = 0.14f) noexcept;

    /** Number of currently active disturbances owned by one water surface. */
    u32 ActiveWaterRippleCount(FNodeId surface) const noexcept {
        return m_Water.ActiveRippleCountForSurface(
            static_cast<u64>(surface.m_Packed));
    }

    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(FRenderContext& context) noexcept override;

private:
    struct FCustomGpuMesh {
        AMeshComponent3D* Component = nullptr;
        FGpuMesh Mesh;
    };

    enum class EWaterGpuState : u8 {
        Unavailable = 0,
        Compiling,
        CpuCompiling,
        PendingCommit,
        Buffering,
        Ready,
        Failed,
    };

    enum class ESkyGpuState : u8 {
        Unavailable = 0,
        Compiling,
        CpuCompiling,
        PendingCommit,
        Ready,
        Failed,
    };

    enum class EShaderGpuState : u8 {
        Unavailable = 0,
        Compiling,
        CpuCompiling,
        PendingCommit,
        Ready,
        Failed,
    };

    /** The one renderer subsystem allowed to publish GPU state this frame. */
    enum class EGpuCommitSubsystem : u8 {
        None = 0,
        Post,
        HdrPbr,
        HdrSsss,
        Subsurface,
        Sky,
        Blit,
        Water,
    };

    struct FWaterDraw {
        const ANode* Node = nullptr;
        const AMeshComponent3D* Mesh = nullptr;
        const AWaterSurface3DComponent* Water = nullptr;
        const FGpuMesh* Gpu = nullptr;
    };

    bool EnsureGpu(FRenderContext& context) noexcept;
    bool EnsureHdrFrameResources(
        IRhiDevice& device, u32 width, u32 height,
        EFormat swapchain_format, EFormat depth_format,
        EGpuCommitSubsystem& frame_commit) noexcept;
    bool BeginSkyCpuCompilation() noexcept;
    bool BeginWaterCpuCompilation() noexcept;
    bool BeginHdrPbrCpuCompilation(
        IRhiDevice& device,
        EFormat rt_format,
        EFormat depth_format) noexcept;
    bool BeginHdrSsssCpuCompilation(
        IRhiDevice& device,
        EFormat rt_format,
        EFormat depth_format) noexcept;
    bool BeginSubsurfaceCpuCompilation(
        IRhiDevice& device) noexcept;
    bool BeginPostCpuCompilation() noexcept;
    bool BeginBlitCpuCompilation() noexcept;
    static void SkyCpuCompileWorkerEntry(void* user) noexcept;
    static void WaterCpuCompileWorkerEntry(void* user) noexcept;
    static void HdrPbrCpuCompileWorkerEntry(void* user) noexcept;
    static void HdrSsssCpuCompileWorkerEntry(void* user) noexcept;
    static void SubsurfaceCpuCompileWorkerEntry(void* user) noexcept;
    static void PostCpuCompileWorkerEntry(void* user) noexcept;
    static void BlitCpuCompileWorkerEntry(void* user) noexcept;
    void JoinCpuCompileWorkers() noexcept;
    static bool TryClaimGpuCommit(
        EGpuCommitSubsystem& frame_commit,
        EGpuCommitSubsystem subsystem) noexcept;
    void AdvanceSkyInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit) noexcept;
    void AdvanceWaterInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit,
        bool scene_has_water) noexcept;
    void AdvanceHdrPbrInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit) noexcept;
    void AdvanceHdrSsssInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit,
        bool scene_needs_subsurface) noexcept;
    void AdvanceSubsurfaceInitialization(
        IRhiDevice& device, u32 width, u32 height,
        EGpuCommitSubsystem& frame_commit,
        bool scene_needs_subsurface) noexcept;
    void EnsureSubsurfaceAuxTargets(
        IRhiDevice& device, u32 width, u32 height,
        EGpuCommitSubsystem& frame_commit,
        bool scene_needs_subsurface) noexcept;
    void AdvancePostInitialization(
        IRhiDevice& device, u32 width, u32 height,
        EFormat swapchain_format,
        EGpuCommitSubsystem& frame_commit) noexcept;
    void AdvanceBlitInitialization(
        IRhiDevice& device,
        EGpuCommitSubsystem& frame_commit,
        bool requested) noexcept;
    bool UploadGraphMeshes(IRhiDevice& device) noexcept;
    void DrainAndReleaseGpu() noexcept;
    void ReleaseGpu() noexcept;
    void UpdateCameraProjection(u32 width, u32 height) noexcept;
    void UpdateCameraView() noexcept;

    /** シーンに置かれた光を集め直す (毎フレーム、描画の前に呼ぶ)。 */
    void CollectSceneLights() noexcept;

    /**
     * 空の太陽を、いま使っている光へ合わせる (毎フレーム)。
     *
     * @details
     * 空に描かれる太陽の位置と、物の陰りを作る光は同じものでなければならない。
     * 別々に持つと、太陽が右にあるのに影が右へ伸びる、といった画になる。
     */
    void UpdateSkyFromSun() noexcept;

    /**
     * 影の描き込み先を用意する (一度だけ)。
     *
     * @param device 生成に使うデバイス。
     * @return 使える状態なら true。
     */
    bool EnsureShadowMap(IRhiDevice& device) noexcept;

    /**
     * 空から環境光を焼く (必要なときだけ)。
     *
     * @details
     * IBL が無いと環境光が «一定の暗い色» になり、陰の側がのっぺり潰れる。空を映した
     * 環境光を入れると、上を向いた面は空の色を、下を向いた面は地面の色を受ける。
     *
     * 焼き直しは重いので、**太陽が十分に動いたときだけ**やり直す。
     * @param device 生成に使うデバイス。
     * @param command_list 焼き込みに使うコマンドリスト。
     * @return 使える状態なら true。
     */
    bool EnsureEnvironmentLighting(IRhiDevice& device, IRhiCommandList& command_list) noexcept;

    /**
     * 空を描く。
     *
     * @details
     * 環境光を焼けていれば、**それと同じ cubemap** を空として描く。見えている空と
     * 物を照らす光が完全に一致するので、«空は晴れているのに陰りが曇り» のような
     * ちぐはぐが起きない。焼けていないときだけ解析的な空 (`CSky`) へ落ちる。
     *
     * 太陽そのものは cubemap に焼かず、画面の解像度で描く (焼くと低解像度の
     * テクセルが四角く拡大されて見える)。
     * @param device 描画に使うデバイス。
     * @param command_list コマンドを積む先。
     * @param color_format 描画先の色形式。
     * @param depth_format 描画先の深度形式。
     */
    /**
     * 雲を計算する (描画パスの外で呼ぶ)。
     *
     * @details
     * 結果は雲自身のテクスチャへ書かれる。画面へ乗せるのは `CompositeClouds`。
     * 分かれているのは、**手前にある物で雲を隠す**のに完成したシーンの深度が要るため。
     * @param device 生成に使うデバイス。
     * @param command_list コマンドを積む先。
     * @param width 画面の幅。
     * @param height 画面の高さ。
     */
    void RenderClouds(IRhiDevice& device, IRhiCommandList& command_list,
                      u32 width, u32 height) noexcept;

    /**
     * 計算した雲を画面へ乗せる。
     *
     * @param command_list コマンドを積む先。
     * @param target 乗せる先。
     * @param scene_depth 完成したシーンの深度 (手前の物で雲を隠すのに使う)。
     * @param width 画面の幅。
     * @param height 画面の高さ。
     */
    void CompositeClouds(IRhiCommandList& command_list, IRhiTexture& target,
                         IRhiTexture& scene_depth, u32 width, u32 height) noexcept;

    void RenderSky(IRhiDevice& device, IRhiCommandList& command_list,
                   EFormat color_format, EFormat depth_format) noexcept;

    /**
     * 大気へ渡す太陽の色を返す。
     *
     * @return シーンの光の色 (強さが掛かったまま)。光が無ければ既定。
     */
    FVec3 SunColorForAtmosphere() const noexcept;

    /**
     * 太陽の色を大気の放射輝度へ直す。
     *
     * @details 一番大きい成分を «設定した強さ» とみなし、残りを色味として扱う。
     * @param sun_color 強さの掛かった色。
     * @return 大気へ渡す放射輝度。
     */
    static FVec3 PhysicalSunIntensity(FVec3 sun_color) noexcept;

    /**
     * 太陽から見た深度を描く (影のもと)。
     *
     * @details
     * 影を落とす設定のメッシュだけを、太陽の側から描く。ここで描いた深度を PBR パスが
     * 参照して «その点が太陽から見えるか» を判定する。
     * @param context 描画文脈。
     * @return 描けたら true。
     */
    bool RenderShadowPass(FRenderContext& context) noexcept;

    /**
     * シーン全体を包む球を求める。
     *
     * @details 影の投影範囲を決めるのに使う。広すぎると影が粗く、狭いと端が切れる。
     * @param out_center 中心の入れ先。
     * @param out_radius 半径の入れ先。
     * @return メッシュが 1 つでもあれば true。
     */
    bool ComputeSceneBounds(FVec3& out_center, f32& out_radius) const noexcept;

    /**
     * いま使っている太陽の向きを返す。
     *
     * @details シーンに平行光源があればその 1 灯目、無ければ既定の向き。
     * 物の陰り・水面・空がすべてこれを使う。
     * @return 正規化済みの向き。
     */
    FVec3 SunDirection() const noexcept;

    /**
     * 水面へ渡す太陽の色を返す。
     *
     * @details 水面は同じ太陽をより強く受けるので、倍率を掛けた色を渡す。
     * @return 水面用の色。
     */
    FVec3 SunColorForWater() const noexcept;
    void AdoptLoadedCamera() noexcept;
    bool RefreshAuthoredCameraPose() noexcept;
    const FGpuMesh* GpuMeshFor(const AMeshComponent3D& component) const noexcept;
    u32 CollectWaterDraws(
        FWaterDraw (&draws)[CWaterSurface3D::kMaxTrackedSurfaces],
        IRhiTexture* depth, u32 width, u32 height) const noexcept;
    bool DrawPbrScene(
        FRenderContext& context,
        CPbrShader& shader,
        const FWaterDraw* excluded_water,
        u32 excluded_count,
        bool subsurface_mrt = false) noexcept;
    void DrawWaterScene(
        FRenderContext& context,
        const FWaterDraw* water_draws,
        u32 water_count,
        IRhiTexture& background,
        IRhiTexture& opaque_depth_snapshot) noexcept;
    void DrawWaterFallback(
        FRenderContext& context,
        const FWaterDraw* water_draws,
        u32 water_count) noexcept;
    CPbrShader& ActiveHdrShader() noexcept {
        return m_HdrShaders[m_HdrActiveSlot];
    }
    const CPbrShader& ActiveHdrShader() const noexcept {
        return m_HdrShaders[m_HdrActiveSlot];
    }

    FScene3DLoadResult m_LoadResult{};
    CPbrShader m_HdrShaders[2];
    CPbrShader::FCompiledShaders m_HdrPendingShaders{};
    FThread m_HdrCompileWorker;
    std::atomic<i32> m_HdrCompileWorkerState{0};
    IRhiDevice* m_HdrCompileDevice = nullptr;
    EFormat m_HdrCompileRtFormat = EFormat::R16G16B16A16_Float;
    EFormat m_HdrCompileDepthFormat = EFormat::D32_Float;
    u8 m_HdrActiveSlot = 0u;
    u8 m_HdrPendingSlot = 1u;
    bool m_HdrPendingIsInitialized = false;
    CPbrShader::FCompiledShaders m_HdrSsssPendingShaders{};
    FThread m_HdrSsssCompileWorker;
    std::atomic<i32> m_HdrSsssCompileWorkerState{0};
    IRhiDevice* m_HdrSsssCompileDevice = nullptr;
    EFormat m_HdrSsssCompileRtFormat = EFormat::R16G16B16A16_Float;
    EFormat m_HdrSsssCompileDepthFormat = EFormat::D32_Float;
    u8 m_HdrSsssPendingSlot = 0u;
    bool m_HdrSsssPendingIsInitialized = false;
    CSubsurfaceScattering m_Ssss;
    CSubsurfaceScattering::FCompiledShaders m_SsssPendingShaders{};
    FThread m_SsssCompileWorker;
    std::atomic<i32> m_SsssCompileWorkerState{0};
    IRhiDevice* m_SsssCompileDevice = nullptr;
    bool m_SsssPendingIsInitialized = false;
    TUniquePtr<IRhiTexture> m_SsssDiffuse;
    TUniquePtr<IRhiTexture> m_SsssMaterial;
    TUniquePtr<IRhiTexture> m_SsssNormal;
    TUniquePtr<IRhiTexture> m_SsssPendingDiffuse;
    TUniquePtr<IRhiTexture> m_SsssPendingMaterial;
    TUniquePtr<IRhiTexture> m_SsssPendingNormal;
    CPostProcess m_Post;
    CPostProcess::FCompiledShaders m_PostPendingShaders{};
    FThread m_PostCompileWorker;
    std::atomic<i32> m_PostCompileWorkerState{0};
    CBlit m_Blit;
    CBlit::FCompiledShaders m_BlitPendingShaders{};
    FThread m_BlitCompileWorker;
    std::atomic<i32> m_BlitCompileWorkerState{0};
    CSky m_Sky;
    CSky::FCompiledShaders m_SkyPendingShaders{};
    FThread m_SkyCompileWorker;
    std::atomic<i32> m_SkyCompileWorkerState{0};
    CWaterSurface3D m_Water;
    CWaterSurface3D::FCompiledShaders m_WaterPendingShaders{};
    FThread m_WaterCompileWorker;
    std::atomic<i32> m_WaterCompileWorkerState{0};
    TUniquePtr<IRhiTexture> m_WaterBackground;
    TUniquePtr<IRhiTexture> m_WaterDepthSnapshot;
    FGpuMesh m_Cube;
    FGpuMesh m_Sphere;
    FGpuMesh m_Plane;
    TArray<FCustomGpuMesh> m_CustomMeshes;
    /** シーンに置かれた光。毎フレーム集め直す。1 灯も無ければ既定の太陽を使う。 */
    CLightCollector3D m_Lights;

    /** 物理ベースの大気。環境光の焼き元にする。 */
    CSkyAtmosphere m_Atmosphere;

    /** 本物の雲。空 pass の外で compute を回し、最後に合成する。 */
    CVolumetricClouds m_Clouds;

    /** 雲の設定。 */
    FScene3DClouds m_CloudParams{};

    /** 雲を使える状態にできたか。 */
    bool m_CloudsReady = false;

    /** このフレームで雲を描いたか (描いていなければ合成もしない)。 */
    bool m_CloudsDrawn = false;

    /** 雲を用意した画面の大きさ。変わったら作り直す。 */
    u32 m_CloudsWidth = 0u;

    /** 雲を用意した画面の高さ。 */
    u32 m_CloudsHeight = 0u;

    /** 大気の設定 (太陽以外)。 */
    FAtmosphereParams m_AtmosphereParams{};

    /** 大気の初期化を一度試したか (失敗しても毎フレーム試さない)。 */
    bool m_AtmosphereTried = false;

    /** 空を映した環境光 (irradiance / prefilter / BRDF LUT)。 */
    CImageBasedLighting m_Ibl;

    /** 環境光を焼けたか。 */
    bool m_IblReady = false;

    /** 焼いたときの太陽の向き。ここから十分に動いたら焼き直す。 */
    FVec3 m_IblBakedSunDirection{0.0f, 0.0f, 0.0f};

    /** 太陽から見た深度。影の判定に使う。 */
    CShadowMap m_Shadow;

    /** 影の描き込み先を用意できたか。 */
    bool m_ShadowReady = false;

    /** このフレームで影を描けたか (描けなければ PBR 側も影を切る)。 */
    bool m_ShadowDrawn = false;

    CCamera m_Camera;
    FScene3DCameraState m_AuthoredCamera{};
    bool m_UseAuthoredCamera = false;
    bool m_HasExplicitCameraOverride = false;
    i32 m_ActiveCameraNodeId = -1;
    FVec3 m_Target{0.0f, 0.0f, 0.0f};
    f32 m_Distance = 8.0f;
    f32 m_Yaw = 0.0f;
    f32 m_Pitch = 0.22f;
    f32 m_Time = 0.0f;
    /** 距離で霞ませる霧。 */
    FScene3DFog m_Fog{};

    FPostProcessParams m_PostParams{};
    ESceneProjectionMode m_Projection = ESceneProjectionMode::Perspective;
    EShaderGpuState m_HdrShaderGpuState = EShaderGpuState::Unavailable;
    EShaderGpuState m_HdrSsssGpuState = EShaderGpuState::Unavailable;
    EShaderGpuState m_SsssGpuState = EShaderGpuState::Unavailable;
    EShaderGpuState m_PostGpuState = EShaderGpuState::Unavailable;
    EShaderGpuState m_BlitGpuState = EShaderGpuState::Unavailable;
    ESkyGpuState m_SkyGpuState = ESkyGpuState::Unavailable;
    EWaterGpuState m_WaterGpuState = EWaterGpuState::Unavailable;
    u32 m_FrameWidth = 0u;
    u32 m_FrameHeight = 0u;
    u32 m_BackgroundAttemptWidth = 0u;
    u32 m_BackgroundAttemptHeight = 0u;
    u32 m_DepthAttemptWidth = 0u;
    u32 m_DepthAttemptHeight = 0u;
    u32 m_SsssResizeAttemptWidth = 0u;
    u32 m_SsssResizeAttemptHeight = 0u;
    u32 m_SsssAuxAttemptWidth = 0u;
    u32 m_SsssAuxAttemptHeight = 0u;
    u32 m_SsssPendingAuxWidth = 0u;
    u32 m_SsssPendingAuxHeight = 0u;
    EFormat m_FrameDepthFormat = EFormat::D32_Float;
    bool m_DepthSnapshotFailed = false;
    bool m_SsssRequested = false;
    bool m_GpuReady = false;
    bool m_GpuAttempted = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FLegacyScene3DAdapter = ALegacyScene3DAdapter;

} // namespace acs::game
