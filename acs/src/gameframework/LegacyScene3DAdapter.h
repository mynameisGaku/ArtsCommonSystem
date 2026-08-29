// SPDX-License-Identifier: Apache-2.0
#pragma once

// 旧ACS3D editor文書をASceneへ可逆的に載せるhost。

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Scene.h"
#include "gameframework/LightCollector3D.h"
#include "gameframework/OrbitCameraInputActionSet3D.h"
#include "gameframework/OrbitCameraController3D.h"
#include "render/ShadowMap.h"
#include "render/Ibl.h"
#include "render/MotionVector.h"
#include "render/Ssao.h"
#include "render/Ssgi.h"
#include "render/Ssr.h"
#include "render/HiZ.h"
#include "render/SkinnedShader.h"
#include "asset/MeshAsset.h"
#include "gameframework/SkinnedMeshComponent3D.h"
#include "render/Atmosphere.h"
#include "gameframework/SceneNodeGraph.h"
#include "gameframework/Scene3DSerialize.h"
#include "gameframework/Scene3DGlobalIllumination.h"
#include "math/Camera.h"
#include "math/Collision3D.h"   // FRay3 / FRayHit3 (RaycastWater と mesh 交差)
#include "render/Blit.h"
#include "render/PbrShader.h"
#include "render/PostProcess.h"
#include "render/RendererFrameEndResult.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/Sprite3DRenderer.h"
#include "render/SubsurfaceScattering.h"
#include "render/WaterSurface3D.h"
#include "threading/Thread.h"

#include <atomic>

namespace acs::game {

class IAssetPackReader;
class AMeshComponent3D;
class ASprite3DComponent;
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
 * 画面空間の遮蔽 (SSAO / GTAO) の設定。
 *
 * @details
 * **物が «床に乗っている» ように見えるかは、ほぼこれで決まる。** 平行光源と環境光だけでは、
 * 物と床が接するところに何も落ちない。影の地図は解像度の都合で接地点まで届かず、
 * 置いてあるのか浮いているのかが読めなくなる。
 *
 * 同じパスで contact shadow (太陽方向への短い march) も出る。
 */
struct FScene3DAmbientOcclusion {
    /**
     * 遮蔽の強さ。0 で切る。
     *
     * @details 1.0 が素直な強さ。上げると «汚れ» に見え始める。
     */
    f32 Intensity = 1.0f;

    /**
     * 遮蔽を探す最大の半径 (世界の単位)。
     *
     * @details
     * **場面の大きさに合わせる。** 人が立つ場面なら 0.5 前後、机の上なら 0.1、
     * 街なら数メートル。大きすぎると陰が広く薄くのび、小さすぎると何も出ない。
     */
    f32 Radius = 0.5f;
};

/**
 * 画面空間の反射 (SSR) の設定。
 *
 * @details
 * 磨いた床・濡れた地面・金属に、画面に映っているものを映す。**«綺麗さ» の印象を
 * いちばん変えるのはこれ。** 粗い面ほど自動で寄与が下がるので、全部がテカることはない。
 *
 * 画面に映っていないものは映せない。画面の外や、物の裏に隠れたものは反射に出ない。
 * 視線を大きく振ると端が伸びて見えることがあるのはそのため。
 */
struct FScene3DReflections {
    /**
     * 反射の強さ。0 で切る。
     *
     * @details 0.6 が素直な強さ。1.0 を超えると «鏡» に寄っていく。
     */
    f32 Intensity = 0.0f;
};

/**
 * 下層の上に重ねる、もう 1 枚の高い雲。
 *
 * @details
 * 雲が 1 枚だけだと、空の «高さ» が読めない。上に薄い雲を敷くと、同じ雲でも
 * 高度差が見えるようになる。
 *
 * `TopAltitude <= BaseAltitude` なら無効 (既定)。
 */
struct FScene3DUpperClouds {
    /** 上層の底。下層の `TopAltitude` より上に置くこと。 */
    f32 BaseAltitude = 0.0f;

    /** 上層の天井。`BaseAltitude` 以下なら出ない。 */
    f32 TopAltitude = 0.0f;

    /** 下層の被覆に対する割合。1.0 にすると空が閉じる。 */
    f32 CoverageScale = 0.55f;

    /** 下層の濃さに対する割合。低いほど透ける。 */
    f32 DensityScale = 0.30f;
};

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
     * @details
     * 雲は画面より小さい寸法でレイマーチし、時間方向の再構成で埋める。この値はその倍率で、
     * **1.0 で画面の 1/4、2.0 で 1/2、4.0 で等倍**。
     *
     * **ドット感の正体はここ。** 参照描画 (`bReferenceMode`) と見比べて、ドットが消えるなら
     * 原因はライティングではなくこの解像度。
     *
     * 既定の 3.0 は、目視でドットが見えなくなった値。軽くしたいなら下げてよいが、
     * 1.0 (1/4) まで落とすと目に見えて粗くなる。
     */
    f32 RenderScale = 3.0f;

    /**
     * 正解画像を作るための参照描画にするか。
     *
     * @details
     * 等倍でレイマーチし、時間方向の再構成を切り、刻みを細かくする。**遊ぶには重すぎる。**
     *
     * 雲が汚いときに、原因がライティングなのか再構成なのかを切り分けるためのもの。
     * - 参照でも汚い → 密度かライティングか大気の側
     * - 参照だけ綺麗 → 低解像度か再構成か履歴の側
     */
    bool bReferenceMode = false;

    /**
     * 雲種と降水成分。既定は手続き生成した天候場を変更しない。
     *
     * @details `CloudTypeInfluence` と `PrecipitationInfluence` を 0 にすると、
     * エンジンが生成した天候場を加工せずに使う。雲種と降水成分を高くすると、
     * 雲柱の高さ変動と上部のかなとこ形状を作る。
     */
    FVolumetricCloudWeather Weather{};

    /**
     * 雲の形と照明をPBR環境光へ反映するか。
     *
     * @details 既定はtrue。被覆が正なら、初回有効化・無効化・太陽方向の有意な変化は
     * 即時、連続する設定・照明変化は最大30成功雲frameごとにまとめてGPU上の
     * 環境cubemapを作り直す。雲の移流だけでは高価なIBLを再生成しない。
     * falseなら表示中の雲と雲影は保ち、環境光だけ従来の雲なし大気へ戻す。
     */
    bool bAffectEnvironmentLighting = true;

    /**
     * 雲の照らし方。位相・消散・多重散乱・環境光・地面からの照り返し。
     *
     * @details
     * 既定のままでよい。触るのは、時間帯や作品の雰囲気に雲の質感を寄せたいときだけ。
     *
     * `SunTransmittance` と `SkyZenithColor` の 2 つは**毎フレーム上書きされる**。前者は
     * 太陽光が雲へ届くまでに大気で失う分、後者は空の天頂色で、どちらも場面の状態から
     * 決まるため。ここへ書いても残らない。
     */
    FVolumetricCloudLighting Lighting{};

    /**
     * どこまで雲を描くか。**「雲が重い」ときに最初に触るところ。**
     *
     * @details
     * 既定は «地平線の果てまで» (250 km) で、ゲームには広すぎる。地上の場面なら
     * `MaxDistance` を 40〜80 km に落とすと、遠くのちらつく細かい雲が消えて軽くもなる。
     *
     * それでも足りなければ `StepGrowth` を 1.0 前後、最後に `ViewSteps` を下げる。
     */
    FVolumetricCloudRange Range{
        /*MaxDistance =*/ 60000.0f,
        /*FadeFraction =*/ 0.35f,
        /*StepGrowth =*/ 1.0f,
        /*ViewSteps =*/ 0u};

    /**
     * 上に重ねる高い雲。**空に高さを出すのはこれ。**
     *
     * @details
     * 既定は無効。使うなら `BaseAltitude`/`TopAltitude` より上に置く。
     *
     * ```cpp
     * Clouds().UpperLayer.BaseAltitude = 7000.0f;
     * Clouds().UpperLayer.TopAltitude  = 9000.0f;
     * ```
     *
     * 1 本のレイで両方を通るので、2 倍にはならない。
     */
    FScene3DUpperClouds UpperLayer{};
};

/**
 * 影の設定。
 *
 * @details
 * **カスケード (CSM) を使うかどうかがいちばん効く。** 影は太陽から見た深度の «写真» で、
 * 1 枚で広い範囲を覆うと、手前の物の影が階段状になる。近くを細かく、遠くを粗く、と
 * 分けて撮るのがカスケード。
 *
 * 狭い場面 (机の上、1 部屋) なら 1 枚で足りる。屋外なら 3〜4 枚。
 */
struct FScene3DShadows {
    /**
     * 何枚に分けるか (1〜4)。
     *
     * @details
     * 1 なら分けない。**増やすほど描き直す回数が増える** (影に落とす物を枚数ぶん描く)。
     * 変えると影の地図を作り直すので、毎フレーム変えないこと。
     */
    u32 CascadeCount = 4u;

    /**
     * 影を描く距離 (メートル)。
     *
     * @details
     * **カメラの far ではない。** far は数 km あることがあり、そこまで枚数を配ると
     * 手前がすかすかになる。«影が要る範囲» を指定する。
     *
     * これより遠い物は影を落とさない。広げるほど手前が粗くなる。
     */
    f32 Distance = 120.0f;

    /**
     * 分割の寄せ方 (0〜1)。
     *
     * @details
     * 0 で等間隔、1 で対数 (手前を極端に細かく)。既定の 0.5 は両者の中間。
     * 手前の影がまだ粗いなら上げる。
     */
    f32 SplitBlend = 0.5f;

    /**
     * 深度の下駄。
     *
     * @details
     * **小さすぎると縞 (shadow acne)、大きすぎると影が浮く (peter-panning)。**
     * 場面の大きさを変えたら見直す値。
     */
    f32 DepthBias = 0.0025f;
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
     * PBR の IBL、SH9、光プローブへ掛ける環境光倍率を返す。
     *
     * @details 既定値は 1.0。direct light、emissive、UI、SSGI、lightmap、SSR には影響しない。
     * @return 有限かつ 0 以上の環境光倍率。
     */
    f32 EnvironmentLightMultiplier() const noexcept {
        return m_EnvironmentLightMultiplier;
    }

    /**
     * PBR の環境由来の間接光だけへ掛ける明るさ倍率を設定する。
     *
     * @details 0 で環境光を消し、有限な正値は上限を設けず受理する。NaN、無限大、負数は
     * 無視して現在値を維持する。CWeatherSystem::AmbientLightMultiplier() をそのまま渡せる。
     * @param multiplier 設定する有限かつ 0 以上の環境光倍率。
     */
    void SetEnvironmentLightMultiplier(f32 multiplier) noexcept;

    /**
     * 雲の設定を触る。
     *
     * @details
     * `Coverage` を 0 にすると出ない (既定)。出すと、太陽の側が明るく縁が光る本物の雲になる。
     * `bAffectEnvironmentLighting` がtrueなら、同じ密度場と照明をPBRの環境光にも反映する。
     * 画面の雲とワールド雲影はこの設定に関係なく従来どおり描く。
     * @return 雲の設定 (次のフレームから効く)。
     */
    FScene3DClouds& Clouds() noexcept { return m_CloudParams; }

    /**
     * 影の設定を触る。
     *
     * @details
     * **影が階段状なら、まず `Distance` を狭める。** 次に `CascadeCount` を増やす。
     *
     * @return 影の設定。`CascadeCount` を変えると次のフレームで地図を作り直す。
     */
    FScene3DShadows& Shadows() noexcept { return m_ShadowParams; }

    /**
     * 影の設定を読む。
     *
     * @return 現在の設定。
     */
    const FScene3DShadows& Shadows() const noexcept { return m_ShadowParams; }

    /**
     * 遮蔽 (SSAO) の設定を触る。
     *
     * @details
     * **物が «床に乗っている» ように見えるかは、ほぼここで決まる。** 平行光源と環境光だけだと、
     * 物と床の接するところに何も落ちず、置いてあるのか浮いているのか読めない。
     *
     * @return 遮蔽の設定 (次のフレームから効く)。
     */
    FScene3DAmbientOcclusion& AmbientOcclusion() noexcept { return m_SsaoParams; }

    /**
     * 遮蔽の設定を読む。
     *
     * @return 現在の設定。
     */
    const FScene3DAmbientOcclusion& AmbientOcclusion() const noexcept {
        return m_SsaoParams;
    }

    /**
     * 反射 (SSR) の設定を触る。
     *
     * @details
     * 既定は 0 (切ってある)。**画面に映っていないものは映せない**ので、切っておいた方が
     * 素直な場面もある。磨いた床や濡れた地面があるなら 0.6 前後から。
     *
     * @return 反射の設定 (次のフレームから効く)。
     */
    FScene3DReflections& Reflections() noexcept { return m_SsrParams; }

    /**
     * 反射の設定を読む。
     *
     * @return 現在の設定。
     */
    const FScene3DReflections& Reflections() const noexcept { return m_SsrParams; }

    /**
     * 画面空間の間接光 (SSGI) の設定を触る。
     *
     * @details
     * `Intensity` を正の値にすると有効になる。出力は内部で保持され、次のフレームの
     * PBR にだけ渡す。無効化、リサイズ、入力不足時は古い間接光を公開しない。
     * @return SSGI の設定 (次のフレームから効く)。
     */
    FScene3DGlobalIllumination& GlobalIllumination() noexcept {
        return m_SsgiParams;
    }

    /** SSGI の設定を読む。 */
    const FScene3DGlobalIllumination& GlobalIllumination() const noexcept {
        return m_SsgiParams;
    }

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
     * 深度に応じた物理大気の空気遠近を有効にする。
     *
     * @details 既定は false で、従来の描画結果と計算負荷を維持する。有効時は不透明物と
     * 水面を camera からの距離に応じて大気へ馴染ませる。`Fog()` は別の表現として従来どおり
     * PBR surface fog にだけ適用し、空気遠近の体積へ重ねて積分しない。
     * @param enabled true なら次の描画から物理大気の空気遠近を使う。
     */
    void SetAerialPerspectiveEnabled(bool enabled) noexcept {
        m_AerialPerspectiveEnabled = enabled;
    }

    /**
     * 物理大気の空気遠近を要求しているか返す。
     *
     * @return `SetAerialPerspectiveEnabled(true)` が設定されていれば true。
     */
    bool AerialPerspectiveEnabled() const noexcept {
        return m_AerialPerspectiveEnabled;
    }

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
     * 一つのnode subtreeへdepth-awareな選択輪郭を設定する。
     *
     * @details subtree_root自身と可視・有効な子孫meshを対象にする。既定では選択なしで、
     * stale/未知handleまたは範囲外設定ではfalseを返し、現在の選択を変更しない。
     * hidden/disabled/destroyed node、mask前段またはpost資源の失敗時は通常の3D表示を維持する。
     * @param subtree_root 輪郭を付けるsubtree root。
     * @param color sRGB表示域の輪郭色。各成分は0以上1以下。
     * @param intensity 輪郭の強さ。0より大きく4以下。
     * @param thickness_pixels 輪郭幅。0より大きく4 pixel以下。
     * @return 設定を受理した場合だけtrue。
     */
    bool SetSelectionHighlight(FNodeId subtree_root, FVec3 color = FVec3{1.0f, 0.66f, 0.16f}, f32 intensity = 1.0f, f32 thickness_pixels = 2.0f) noexcept;

    /** 選択輪郭を解除する。未設定でも安全に呼べる。 */
    void ClearSelectionHighlight() noexcept;

    /**
     * 現在設定されている選択輪郭のsubtree rootを返す。
     *
     * @return 設定中の有効なFNodeId。未設定または破棄済みならinvalid。
     */
    FNodeId SelectionHighlightNode() const noexcept;

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

    /**
     * 自由カメラのキー操作 (矢印・WASD・Q/E・PageUp/PageDown・Escape) を受け付けるか決める。
     *
     * @details
     * **既定は受け付ける。** ただしこれは編集中に見回すためのもので、入れたままだと
     * 同じ物理キーで自由カメラも動き、Escape はゲームを終了する。自分でカメラを動かすなら切ること。
     * 移動と回転とzoomは固定tickだけで進むため、CGameの固定timestepを無効にすると停止する。
     * 切替時は補間履歴を現在状態へ揃え、古い移動区間を再表示しない。
     *
     * 撮り比べのときも切る。キーが押されているだけで画角が変わり、比較にならない。
     * @param enabled 受け付けるなら true。
     */
    void SetFreeCameraEnabled(bool enabled) noexcept;

    /**
     * 自由カメラのキー操作を受け付けるかを返す。
     *
     * @return 受け付けるなら true。
     */
    bool FreeCameraEnabled() const noexcept { return m_FreeCameraEnabled; }

    /** 自由cameraのpresentation障害物回避設定。 */
    struct FOrbitCameraObstructionSettings3D final {
        /** 有効なscene meshでcamera距離を短縮するならtrue。既定は互換性のためfalse。 */
        bool Enabled = false;

        /** targetからこの距離未満のhitを追従対象として除外する。 */
        f32 TargetClearance = 0.75f;

        /** cameraを障害物の手前へ離す距離。 */
        f32 CameraClearance = 0.25f;

        /** 点rayの代わりに移動させるworld空間camera probe半径。0なら従来ray。 */
        f32 ProbeRadius = 0.0f;

        /** 障害物から離れる一秒あたりの指数復帰速度。0なら従来互換の即時復帰。 */
        f32 RecoverySharpness = 0.0f;
    };

    /**
     * presentation障害物回避設定を検証し、成功時だけ反映する。
     * @param settings 有効状態、target除外距離、camera余白、probe半径、復帰速度。
     * @return 全値が有限でprobe半径と復帰速度が0以上、余白後もcontrollerの最小距離を保てるならtrue。
     */
    bool TrySetOrbitCameraObstructionSettings(const FOrbitCameraObstructionSettings3D& settings) noexcept;

    /** 現在の検証済みpresentation障害物回避設定を返す。 */
    const FOrbitCameraObstructionSettings3D& OrbitCameraObstructionSettings() const noexcept { return m_OrbitCameraObstructionSettings; }

    /**
     * 見る向きと距離を決める。
     *
     * @details 自由カメラが有効なままだと、次のキー操作で上書きされる。
     * @param target 見る点。
     * @param yaw 水平の向き (ラジアン)。
     * @param pitch 上下の向き (ラジアン、正で見下ろし)。
     * @param distance 見る点からの距離。
     */
    void SetOrbit(FVec3 target, f32 yaw, f32 pitch, f32 distance) noexcept;

    /**
     * 自由cameraの固定tick補間区間をprocess内snapshotへ複製する。
     * @param output previous/current状態の書き込み先。失敗時は変更しない。
     * @return 両状態が現在のcamera設定で有効ならtrue。
     */
    bool TryCaptureOrbitCameraSnapshot(COrbitCameraController3D::FOrbitCameraFixedStepSnapshot3D& output) const noexcept;

    /**
     * 自由cameraの固定tick補間区間をsnapshotから一括復元する。
     * @param snapshot 復元するprevious/current状態。
     * @return 両状態が有効ならtrue。失敗時はcamera状態とviewを変更しない。
     */
    bool TryRestoreOrbitCameraSnapshot(const COrbitCameraController3D::FOrbitCameraFixedStepSnapshot3D& snapshot) noexcept;

    /** 単体previewとgameplayで使う自由cameraを返す。 */
    CCamera& Camera() noexcept { return m_Camera; }

    /** Read-only standalone camera. */
    const CCamera& Camera() const noexcept { return m_Camera; }

    /**
     * authored cameraの有無にかかわらず、orbit cameraを明示的に選ぶかを設定する。
     *
     * @details trueでは毎frameのcamera再選択を抑止し、falseでは明示camera指定を解除して
     * deterministic authored camera選択へ戻す。authored cameraが無い場合はorbit cameraを維持する。
     * 切替時はorbit cameraの補間区間を現在状態へ揃え、古い表示区間を残さない。
     * @param active orbit cameraを明示選択するならtrue。
     */
    void SetOrbitCameraActive(bool active) noexcept;

    /**
     * 現在の描画cameraがorbit cameraならtrueを返す。
     *
     * @details 明示選択だけでなく、authored cameraが無い自動代替もtrueになる。
     * @return orbit cameraを使っているならtrue。
     */
    bool OrbitCameraActive() const noexcept { return !m_UseAuthoredCamera; }

    /**
     * orbit cameraを明示的に選択しているならtrueを返す。
     *
     * @details authored camera不在による自動代替はfalse。Bind前の選択modeを復元する用途で使う。
     * @return SetOrbitCameraActive(true)による明示overrideが有効ならtrue。
     */
    bool OrbitCameraOverrideActive() const noexcept;

    /**
     * authored cameraをstable idまたはnode idで明示選択しているならtrueを返す。
     *
     * @details serialized cameraはAuthoredCamera()->NodeId、NodeIdが負のruntime cameraは
     * AuthoredCamera()->StableIdを保存し、対応するSetActiveCamera overloadで復元できる。
     * @return SetActiveCamera成功による明示overrideが有効ならtrue。
     */
    bool AuthoredCameraOverrideActive() const noexcept;

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

    /**
     * このシーンが直前に記録した雲命令へ、実際のGPU提出結果を返す公開アダプター。
     * CGameの標準経路はID付きで自動通知する。独自描画ホストが結果を遅延または
     * 再送する場合は、別候補への誤適用を防ぐためID付き入口を使う。
     */
    void ResolveFrameSubmission(bool submitted) noexcept;

    /**
     * 指定した提出IDの雲候補だけへGPU提出結果を返す公開アダプター。
     * 遅延・重複通知はfalseを返して現在候補を変更しない。
     */
    bool ResolveFrameSubmission(u64 submission_id, bool submitted) noexcept;

    /** 固定tick自由カメラへ使うscene入力サービスを要求する。 */
    ESvc WantedServices() const noexcept override
    {
        return ESvc::Input;
    }

    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    /** 基底更新を行い、派生sceneのOnTickを毎frame一度だけ呼ぶ。 */
    void OnUpdate(f32 dt) noexcept override;
    /** scene入力の6 actionから自由カメラを固定刻みで更新する。 */
    void OnFixedUpdate(f32 fixed_dt) noexcept override;
    void OnRender(FRenderContext& context) noexcept override;

protected:
    /** World提出通知へこの3Dシーンを登録する。 */
    void OnWorldSubsystemsReady_Internal() noexcept override;

    /**
     * 透明 3D 追加描画へ渡す、そのフレームだけの値コンテキスト。
     *
     * @details
     * OnRenderTransparent3D の呼出し中は ColorTarget が既存内容を保持した load 状態で、
     * DepthTarget が null でなければ同じ深度バッファも DSV として bind されている。
     * 参照とポインタは所有せず、フックの呼出し中だけ使う。
     */
    struct FScene3DTransparentRenderContext final {
        /** 現在の RHI device。フックの呼出し中だけ参照する。 */
        IRhiDevice& Device;

        /** 現在記録中の RHI command list。フックの呼出し中だけ参照する。 */
        IRhiCommandList& Commands;

        /** 描画時点の active camera。行列と位置を参照する。 */
        const CCamera& Camera;

        /** 追加描画先の HDR color target。load-bind 済みである。 */
        IRhiTexture& ColorTarget;

        /** 追加描画で読み取り可能な深度 target (無い場合は null)。 */
        IRhiTexture* DepthTarget = nullptr;

        /** target の幅 (ピクセル)。 */
        u32 Width = 0u;

        /** target の高さ (ピクセル)。 */
        u32 Height = 0u;

    };

    /**
     * HDR シーンへ透明 3D 描画を追加する。
     *
     * @details
     * 既定実装は何もしないため、外部描画を使わない派生 scene は安全にそのまま動く。
     * 基底が Load/状態復旧/End を管理するので、派生は context の対象へ描画だけを追加する。
     * @param context load-bind 済み HDR/深度と camera を含む一時コンテキスト。
     */
    virtual bool OnRenderTransparent3D(
        const FScene3DTransparentRenderContext& context) noexcept
    {
        (void)context;
        return false;
    }

private:
    /** Worldの提出通知へ未登録なら再試行し、登録済みならtrueを返す。 */
    bool TryBindFrameSubmission_Internal() noexcept;

    /** World提出通知を公開アダプターへ転送するlistener関数。 */
    static void ResolveFrameSubmissionCallback(
        void* listener,
        const FRendererFrameEndResult& result) noexcept;

    struct FCustomGpuMesh {
        AMeshComponent3D* Component = nullptr;
        FGpuMesh Mesh;
    };

    /** sceneコンポーネントと所有GPU画像の対応。 */
    struct FCustomGpuSprite {
        /** 画像を使うnode。世代を含むhandleなので破棄後の再利用と衝突しない。 */
        FNodeId Node;

        /** GPU画像の生成元。差し替え検出とCPU画像の寿命保持に使う。 */
        TSharedPtr<AAsset> SourceImage;

        /** 描画アダプターが単独所有するGPU画像。 */
        TUniquePtr<IRhiTexture> Texture;
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
        Sprite,
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
    bool BeginSpriteCpuCompilation() noexcept;
    static void SkyCpuCompileWorkerEntry(void* user) noexcept;
    static void WaterCpuCompileWorkerEntry(void* user) noexcept;
    static void HdrPbrCpuCompileWorkerEntry(void* user) noexcept;
    static void HdrSsssCpuCompileWorkerEntry(void* user) noexcept;
    static void SubsurfaceCpuCompileWorkerEntry(void* user) noexcept;
    static void PostCpuCompileWorkerEntry(void* user) noexcept;
    static void BlitCpuCompileWorkerEntry(void* user) noexcept;
    static void SpriteCpuCompileWorkerEntry(void* user) noexcept;
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
    void AdvanceSpriteInitialization(IRhiDevice& device, EFormat depth_format, EGpuCommitSubsystem& frame_commit, bool requested) noexcept;
    bool UploadGraphMeshes(IRhiDevice& device) noexcept;
    /** 現在のgraphとGPU画像表を登録順に線形比較し、同一ならtrueを返す。 */
    bool SpriteResourcesMatchGraph_Internal(const ANode& node, usize& matched_count, u32 depth = 0u) const noexcept;
    /** 3Dスプライトの追加・除去・画像差し替えをGPU画像表へ反映する。 */
    bool SynchronizeGraphSprites_Internal(IRhiDevice& device) noexcept;
    void DrainAndReleaseGpu() noexcept;
    void ReleaseGpu() noexcept;
    void UpdateCameraProjection(u32 width, u32 height) noexcept;
    void UpdateCameraView() noexcept;
    /** 指定したorbit状態をcameraと表示用状態へ反映する内部処理。 */
    void UpdateOrbitCameraView_Internal(const COrbitCameraController3D::FOrbitCameraState3D& state, f32 recovery_delta_seconds) noexcept;
    /** scene graphの有効meshからpresentation用の衝突回避状態を求める内部処理。 */
    bool TryResolveOrbitCameraObstruction_Internal(const COrbitCameraController3D::FOrbitCameraState3D& state, COrbitCameraController3D::FOrbitCameraState3D& output) const noexcept;
    /** 固定時計の補間率から今回描画するcamera状態を反映する内部処理。 */
    void UpdatePresentedCameraView_Internal(f32 recovery_delta_seconds) noexcept;

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
     * 遮蔽 (SSAO) の描き込み先を用意する。画面の大きさが変わったら作り直す。
     *
     * @param device 生成に使うデバイス。
     * @param width 画面の幅。
     * @param height 画面の高さ。
     * @return 使える状態なら true。
     */
    bool EnsureAmbientOcclusion(IRhiDevice& device, u32 width, u32 height) noexcept;

    /**
     * SSAO、SSR、SSGI が共有する法線・深度前段を用意する。
     *
     * @param device 描画先を作るRHIデバイス。
     * @param width 画面の幅。
     * @param height 画面の高さ。
     * @return 前段を使える状態なら true。
     */
    bool EnsureNormalDepth(IRhiDevice& device, u32 width, u32 height) noexcept;

    /**
     * 遮蔽を計算する前段として、法線と深度を先に描く。
     *
     * @details
     * `CSsao` は «同じ幾何の法線と深度» を要求する。`CMotionVector` のパスが
     * その 2 つを 1 度で書くので、それを使う。
     *
     * @param context 描画文脈。
     * @return 全部描けたら true。1 つでも欠けたら false (欠けた遮蔽は使わない)。
     */
    bool RenderNormalDepthPrepass(FRenderContext& context) noexcept;

    /**
     * 前段の法線と深度から遮蔽を計算する。
     *
     * @param device 描画に使うデバイス。
     * @param context 描画文脈。
     * @return 遮蔽が使える状態になったら true。
     */
    bool RenderAmbientOcclusionPass(IRhiDevice& device, FRenderContext& context) noexcept;

    /** SSGI の出力RTと時間履歴を、必要な解像度で用意する。 */
    bool EnsureGlobalIllumination(
        IRhiDevice& device, u32 width, u32 height) noexcept;

    /**
     * 完成したHDR色からSSGIを計算し、次フレーム用の結果を公開する。
     *
     * @param device 描画に使うRHIデバイス。
     * @param context 描画文脈。
     * @param scene_color 現フレームの完成したHDR色。
     * @param scene_depth 現フレームのshader-visible深度。
     * @return 次フレームへ渡せる出力を作れたら true。
     */
    bool RenderGlobalIlluminationPass(
        IRhiDevice& device, FRenderContext& context,
        IRhiTexture& scene_color, IRhiTexture& scene_depth) noexcept;

    /** SSGI の出力を無効化し、次回復帰時を履歴の初回に戻す。 */
    void InvalidateGlobalIlluminationOutput() noexcept;

    /**
     * 反射 (SSR) の描き込み先を用意する。画面の大きさが変わったら作り直す。
     *
     * @param device 生成に使うデバイス。
     * @param hdr_format シーンの HDR と同じ形式。
     * @param width 画面の幅。
     * @param height 画面の高さ。
     * @return 使える状態なら true。
     */
    bool EnsureReflections(IRhiDevice& device, EFormat hdr_format,
                           u32 width, u32 height) noexcept;

    /**
     * 完成したシーンから反射を作る。
     *
     * @details
     * **これは «次の» フレームのための仕事。** 反射を混ぜるのは PBR パスなので、同じ
     * フレームの結果は間に合わない。1 フレーム遅れるが、SSR は元々時間方向に均すので
     * 破綻しない。
     *
     * @param device 描画に使うデバイス。
     * @param context 描画文脈。
     * @param scene_color 完成したシーンの色。
     * @param scene_depth シーンの深度。
     * @return 反射が使える状態になったら true。
     */
    bool RenderReflectionPass(IRhiDevice& device, FRenderContext& context,
                              IRhiTexture& scene_color,
                              IRhiTexture& scene_depth) noexcept;

    /** 骨付きメッシュ 1 体分の入れ物。 */
    struct FSkinnedInstance {
        /** 持ち主の部品 (同一判定に使う。所有はしない)。 */
        const ASkinnedMeshComponent3D* Component = nullptr;

        /** 変形した結果を置く GPU バッファ (頂点は毎フレーム書き換える)。 */
        FGpuMesh Mesh;

        /** 変形の計算に使う CPU 側の作業領域。 */
        TArray<FMeshVertex> Scratch;
    };

    /** このフレームに描く骨付きメッシュ。 */
    struct FSkinnedDraw {
        /** 描く GPU メッシュ (所有はしない)。 */
        const FGpuMesh* Mesh = nullptr;

        /** 置く場所。 */
        FMat4 Model{};

        /** アルベド色。 */
        FVec3 Color{1.0f, 1.0f, 1.0f};

        /** 可視選択subtreeに含まれ、normal alphaへmaskを書くならtrue。 */
        bool SelectionHighlighted = false;
    };

    /**
     * 骨付きメッシュ 1 体分の入れ物を用意する (まだなら)。
     *
     * @details
     * **インスタンスごとに持つ。** 同じモデルでも姿勢が違えば頂点が違うので、
     * アセット単位で共有すると全員が最後の 1 体の姿勢になる。
     *
     * @param device 生成に使うデバイス。
     * @param component 対象の部品。
     * @return 用意できた入れ物。できなければ nullptr。
     */
    FSkinnedInstance* SkinnedInstanceFor(
        IRhiDevice& device, const ASkinnedMeshComponent3D& component) noexcept;

    /**
     * 骨で動くメッシュを CPU で変形し、描く準備を整える。
     *
     * @details
     * **変形した結果を普通の頂点バッファへ入れ直す。** そうすると以降はただのメッシュなので、
     * 影・遮蔽・反射・IBL が静的メッシュと同じように効く。GPU スキニング
     * (CSkinnedShader) より CPU を使うが、あちらは Blinn-Phong で質感が揃わない。
     *
     * 何体も出すなら CPU が効いてくる。そのときは CPbrShader 側へスキニングを足すのが本筋。
     *
     * @param device 描画に使うデバイス。
     * @return 1 体でも用意できたら true。
     */
    bool UpdateSkinnedMeshes(IRhiDevice& device) noexcept;

    /**
     * 空から環境光を焼く (必要なときだけ)。
     *
     * @details
     * IBL が無いと環境光が «一定の暗い色» になり、陰の側がのっぺり潰れる。空を映した
     * 環境光を入れると、上を向いた面は空の色を、下を向いた面は地面の色を受ける。
     *
     * 焼き直しは重いので、太陽方向が十分に動いたときは即時、雲の連続変化は
     * 固定間隔へまとめてやり直す。
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
                      u32 width, u32 height, u64 submission_id) noexcept;

    /**
     * 計算した雲を画面へ乗せる。
     *
     * @param command_list コマンドを積む先。
     * @param target 乗せる先。
     * @param scene_depth 完成したシーンの深度 (手前の物で雲を隠すのに使う)。
     * @param width 画面の幅。
     * @param height 画面の高さ。
     * @param aerial_volume 空気遠近の散乱体積。nullptrなら従来の雲合成へ戻る。
     * @param aerial_transmittance 空気遠近の透過率体積。nullptrなら従来の雲合成へ戻る。
     * @param aerial_max_distance 体積を生成した最大距離 (Legacy 3D world単位のメートル)。
     */
    void CompositeClouds(
        IRhiCommandList& command_list, IRhiTexture& target,
        IRhiTexture& scene_depth, u32 width, u32 height,
        IRhiTexture* aerial_volume,
        IRhiTexture* aerial_transmittance,
        f32 aerial_max_distance) noexcept;

    void RenderSky(IRhiDevice& device, IRhiCommandList& command_list,
                   EFormat color_format, EFormat depth_format) noexcept;

    /**
     * 大気へ渡す太陽の色を返す。
     *
     * @return シーンの光の色 (強さが掛かったまま)。光が無ければ既定。
     */
    FVec3 SunColorForAtmosphere() const noexcept;

    /**
     * 雲へ渡す太陽の色を返す。
     *
     * @return シーンの光の色 (色×強度)。光が無ければEditorと同じ既定値。
     */
    FVec3 SunColorForClouds() const noexcept;

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
    /** 明示的なorbit camera選択を保持しているならtrueを返す。 */
    bool HasExplicitOrbitCameraOverride_Internal() const noexcept;
    /** orbit cameraの補間履歴と障害物回避表示を現在状態へ揃える。 */
    void ResetOrbitCameraPresentation_Internal() noexcept;
    bool RefreshAuthoredCameraPose() noexcept;
    const FGpuMesh* GpuMeshFor(const AMeshComponent3D& component) const noexcept;
    IRhiTexture* TextureFor(FNodeId node, const TSharedPtr<AAsset>& source_image) const noexcept;
    bool DrawSpriteScene(FRenderContext& context) noexcept;
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
    CSprite3DRenderer m_SpriteRenderer;
    CSprite3DRenderer::FCompiledShaders m_SpritePendingShaders{};
    FThread m_SpriteCompileWorker;
    std::atomic<i32> m_SpriteCompileWorkerState{0};
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
    TArray<FCustomGpuSprite> m_CustomSprites;
    TArray<CSprite3DRenderer::FDraw> m_SpriteDraws;
    /** シーンに置かれた光。毎フレーム集め直す。1 灯も無ければ既定の太陽を使う。 */
    CLightCollector3D m_Lights;

    /** 物理ベースの大気。環境光の焼き元にする。 */
    CSkyAtmosphere m_Atmosphere;

    /** 本物の雲。空 pass の外で compute を回し、最後に合成する。 */
    /** 自由カメラのキー操作を受け付けるか。 */
    bool m_FreeCameraEnabled = true;

    CVolumetricClouds m_Clouds;

    /** 雲の設定。 */
    FScene3DClouds m_CloudParams{};
    FScene3DShadows m_ShadowParams{};
    FScene3DAmbientOcclusion m_SsaoParams{};
    FScene3DReflections m_SsrParams{};
    FScene3DGlobalIllumination m_SsgiParams{};

    /** 雲を使える状態にできたか。 */
    bool m_CloudsReady = false;

    /** このフレームで雲を描いたか (描いていなければ合成もしない)。 */
    bool m_CloudsDrawn = false;

    /** 雲を用意した画面の大きさ。変わったら作り直す。 */
    u32 m_CloudsWidth = 0u;

    /** 雲を用意したときが参照描画だったか。切り替わったら作り直す。 */
    bool m_CloudsSizedForReference = false;

    /** 雲を用意した画面の高さ。 */
    u32 m_CloudsHeight = 0u;

    /** 大気の設定 (太陽以外)。 */
    FAtmosphereParams m_AtmosphereParams{};

    /** 大気の初期化を一度試したか (失敗しても毎フレーム試さない)。 */
    bool m_AtmosphereTried = false;

    /** 既存alignment padding内で保持する、物理大気の空気遠近要求。 */
    bool m_AerialPerspectiveEnabled = false;

    /** boolと署名の間の既存alignment paddingで保持する、焼き込み観測者高度の単位。 */
    u16 m_IblBakedObserverAltitudeBucket = 0u;

    /** 環境光へ焼いた雲形状・照明の署名。 */
    u32 m_IblBakedCloudSignature = ~u32{0};

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

    /** 用意してある地図の枚数。設定と違ったら作り直す。 */
    u32 m_ShadowCascadeCount = 0u;

    /** このフレームで影を描けたか (描けなければ PBR 側も影を切る)。 */
    bool m_ShadowDrawn = false;

    /** 法線と深度を先に描くパス。遮蔽の材料になる。 */
    CMotionVector m_NormalDepth;

    /** 法線と深度の前段を用意できたか。SSAOの有効状態とは独立。 */
    bool m_NormalDepthReady = false;

    /** このフレームで法線と深度を完全に描けたか。 */
    bool m_NormalDepthDrawn = false;

    /** 法線と深度を用意してある画面の幅。 */
    u32 m_NormalDepthWidth = 0u;

    /** 法線と深度を用意してある画面の高さ。 */
    u32 m_NormalDepthHeight = 0u;

    /** 画面空間の遮蔽 (GTAO + contact shadow)。 */
    CSsao m_Ssao;

    /** 画面空間の間接光。出力RTと時間履歴を所有する。 */
    CSsgi m_Ssgi;

    /** SSGI の出力RTとパイプラインを用意できたか。 */
    bool m_SsgiReady = false;

    /** 遮蔽の描き込み先を用意できたか。 */
    bool m_SsaoReady = false;

    /** このフレームで遮蔽を計算できたか。 */
    bool m_SsaoDrawn = false;

    /** 用意してある遮蔽の大きさ。画面がこれと違ったら作り直す。 */
    u32 m_SsaoWidth = 0u;

    /** 用意してある遮蔽の大きさ。 */
    u32 m_SsaoHeight = 0u;

    /** 画面空間の反射。 */
    CSsr m_Ssr;

    /**
     * 深度の階層 (Hi-Z)。反射のレイが遠くまで届くようになる。
     *
     * @details
     * 無いと SSR は 1 段だけの粗い探索になり、**ほとんどのレイが何にも当たらない。**
     * 実測でも、これが無い状態では床の 5 % しか変わらなかった。
     */
    CHiZ m_HiZ;

    /** 深度の階層を用意できたか。 */
    bool m_HiZReady = false;

    /** 用意済みの骨付きメッシュ (1 体ごと)。 */
    TArray<FSkinnedInstance> m_SkinnedInstances;

    /** このフレームぶんの描画予定。 */
    TArray<FSkinnedDraw> m_SkinnedDrawn;



    /** 反射の描き込み先を用意できたか。 */
    bool m_SsrReady = false;

    /** 使える反射があるか (前のフレームで作れたか)。 */
    bool m_SsrValid = false;

    /** SSGIの出力を次のPBRへ渡せるか (現フレームの計算前の値)。 */
    bool m_SsgiValid = false;

    /** SSGIを前フレームから継続して有効にしているか。 */
    bool m_SsgiWasEnabled = false;

    /** SSGI出力を用意してある画面の幅。 */
    u32 m_SsgiWidth = 0u;

    /** SSGI出力を用意してある画面の高さ。 */
    u32 m_SsgiHeight = 0u;

    /** 用意してある反射の大きさ。 */
    u32 m_SsrWidth = 0u;

    /** 用意してある反射の大きさ。 */
    u32 m_SsrHeight = 0u;

    /** 前のフレームの view * projection。反射を時間方向に均すのに要る。 */
    FMat4 m_PrevViewProjection{};

    /** 前のフレームの行列を持っているか (初回は持っていない)。 */
    bool m_HasPrevViewProjection = false;

    CCamera m_Camera;
    FScene3DCameraState m_AuthoredCamera{};
    bool m_UseAuthoredCamera = false;
    bool m_HasExplicitCameraOverride = false;
    i32 m_ActiveCameraNodeId = -1;
    /** device非依存の自由camera計算器。 */
    COrbitCameraController3D m_OrbitCameraController{};

    /** scene-localな自由camera presentation障害物回避設定。 */
    FOrbitCameraObstructionSettings3D m_OrbitCameraObstructionSettings{};

    /** scene入力を自由cameraの6操作軸へ変換するaction集合。 */
    FOrbitCameraInputActionSet3D m_OrbitCameraActions{};

    /** 一つ前の固定tick完了時に確定した自由camera状態。 */
    COrbitCameraController3D::FOrbitCameraState3D m_PreviousOrbitCameraState{};

    /** 自由cameraのsnapshot可能なworld状態。 */
    COrbitCameraController3D::FOrbitCameraState3D m_OrbitCameraState{};

    /** viewとprojectionへ最後に反映した補間済み自由camera状態。 */
    COrbitCameraController3D::FOrbitCameraState3D m_PresentedOrbitCameraState{};

    /** 障害物による短縮または外向き復帰を継続中ならtrue。 */
    bool m_IsOrbitCameraObstructionPresentationActive = false;

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
    EShaderGpuState m_SpriteGpuState = EShaderGpuState::Unavailable;
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

    /** 既存field位置を保ち、末尾paddingで所有する環境光倍率。 */
    f32 m_EnvironmentLightMultiplier = 1.0f;

    /** 現在のRHIでBRDF LUTが使えないと確認済みか。 */
    bool m_BrdfLutUnavailable = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FLegacyScene3DAdapter = ALegacyScene3DAdapter;

} // namespace acs::game
