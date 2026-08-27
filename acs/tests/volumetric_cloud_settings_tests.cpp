// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/Sky.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

using namespace acs;

namespace {

/** 指定した描画実装ファイルを文字列として読み込む。 */
std::string ReadRenderFile(const char* filename)
{
    /** このテストソース自身の場所。 */
    const std::filesystem::path testFile{__FILE__};
    /** 読み込み対象となる描画ファイルの場所。 */
    const std::filesystem::path sourcePath = testFile.parent_path().parent_path() / "src" / "render" / filename;
    /** 描画ファイルを読み取る入力ストリーム。 */
    std::ifstream stream(sourcePath, std::ios::binary);
    if (!stream) {
        stream.open(std::filesystem::path{"acs"} / "src" / "render" / filename, std::ios::binary);
    }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/** 文字列が指定した断片を含むか返す。 */
bool Contains(const std::string& value, const char* fragment)
{
    return value.find(fragment) != std::string::npos;
}

/** 2 個の目印に挟まれた実装範囲を返す。 */
std::string SliceBetween(const std::string& value, const char* begin_marker, const char* end_marker)
{
    /** 対象範囲の開始位置。 */
    const std::size_t begin = value.find(begin_marker);
    if (begin == std::string::npos) return {};
    /** 対象範囲の終了位置。 */
    const std::size_t end = value.find(end_marker, begin);
    if (end == std::string::npos || end <= begin) return {};
    return value.substr(begin, end - begin);
}

/** 3 成分がすべて有限か返す。 */
bool IsFinite(FVec3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

ACS_TEST(VolumetricCloudSettings, LightingRejectsNonFiniteAndUnsafeCoefficients)
{
    /** 不正入力を作るための非数。 */
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    /** 不正入力を作るための正の無限大。 */
    const f32 infinity = std::numeric_limits<f32>::infinity();
    /** 境界外の値を含む照明入力。 */
    FVolumetricCloudLighting requested{};
    requested.ViewExtinction = -2.0f;
    requested.LightExtinction = infinity;
    requested.SunScatter = 2.0f;
    requested.SunScatteringLuminanceScale = infinity;
    requested.PowderStrength = -1.0f;
    requested.PhaseForward = 1.0f;
    requested.PhaseBackward = -1.0f;
    requested.PhaseBlend = nan;
    requested.PhaseMin = 20.0f;
    requested.PhaseMax = 2.0f;
    requested.MultiScatterContribution = 3.0f;
    requested.MultiScatterOcclusion = -4.0f;
    requested.SkyZenithColor = FVec3{-1.0f, nan, infinity};
    requested.MultiScatterEccentricity = 5.0f;
    requested.AmbientAtBase = -1.0f;
    requested.AmbientAtTop = 2.0f;
    requested.GroundContribution = nan;
    requested.SunTransmittance = FVec3{-1.0f, 0.4f, infinity};
    requested.GroundColor = FVec3{-1.0f, nan, 1.0e30f};

    /** GPU へ渡せる範囲へ直した照明。 */
    const FVolumetricCloudLighting lighting = SanitizeVolumetricCloudLighting(requested);
    EXPECT_NEAR(lighting.ViewExtinction, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.LightExtinction, 5.0f, 0.0f);
    EXPECT_NEAR(lighting.SunScatter, 1.0f, 0.0f);
    EXPECT_NEAR(lighting.SunScatteringLuminanceScale, 1.0f, 0.0f);
    EXPECT_NEAR(lighting.PowderStrength, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.PhaseForward, kVolumetricCloudMaxPhaseEccentricity, 0.0f);
    EXPECT_NEAR(lighting.PhaseBackward, -kVolumetricCloudMaxPhaseEccentricity, 0.0f);
    EXPECT_NEAR(lighting.PhaseBlend, 0.85f, 0.0f);
    EXPECT_NEAR(lighting.PhaseMin, 2.0f, 0.0f);
    EXPECT_NEAR(lighting.PhaseMax, 20.0f, 0.0f);
    EXPECT_NEAR(lighting.MultiScatterContribution, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.MultiScatterOcclusion, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.MultiScatterEccentricity, kVolumetricCloudMaxPhaseEccentricity, 0.0f);
    EXPECT_NEAR(lighting.AmbientAtBase, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.AmbientAtTop, 1.0f, 0.0f);
    EXPECT_NEAR(lighting.GroundContribution, 0.15f, 0.0f);
    EXPECT_TRUE(IsFinite(lighting.SkyZenithColor));
    EXPECT_TRUE(IsFinite(lighting.SunTransmittance));
    EXPECT_TRUE(IsFinite(lighting.GroundColor));
    EXPECT_NEAR(lighting.SkyZenithColor.x, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.SkyZenithColor.y, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.SkyZenithColor.z, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.SunTransmittance.x, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.SunTransmittance.y, 0.4f, 0.0f);
    EXPECT_NEAR(lighting.SunTransmittance.z, 1.0f, 0.0f);
    EXPECT_NEAR(lighting.GroundColor.x, 0.0f, 0.0f);
    EXPECT_NEAR(lighting.GroundColor.y, 0.19f, 0.0f);
    EXPECT_NEAR(lighting.GroundColor.z, 16384.0f, 0.0f);
}

ACS_TEST(VolumetricCloudSettings, DefaultLightingKeepsWaterDropletScatteringNearConservative)
{
    /** 水滴雲として使う既定照明。 */
    const FVolumetricCloudLighting lighting{};
    // 区間不透明度が消散を含むため、散乱割合を小さくすると雲が灰色の吸収体になる。
    EXPECT_NEAR(lighting.SunScatter, 1.0f, 0.0f);
    // 補償量は媒質と視線で変わるため、未測定の一律増幅を既定にしない。
    EXPECT_NEAR(lighting.SunScatteringLuminanceScale, 1.0f, 0.0f);
    EXPECT_NEAR(lighting.ViewExtinction, lighting.LightExtinction, 0.0f);
}

ACS_TEST(VolumetricCloudSettings, DirectionalLuminanceCompensationIsFiniteAndDoesNotReplaceAlbedo)
{
    /** 有向光の輝度補償だけを上限外にした入力。 */
    FVolumetricCloudLighting excessive{};
    excessive.SunScatteringLuminanceScale = 1000.0f;
    /** HDRの有限範囲へ直した照明。 */
    const FVolumetricCloudLighting bounded =
        SanitizeVolumetricCloudLighting(excessive);
    EXPECT_NEAR(bounded.SunScatteringLuminanceScale, kVolumetricCloudMaxSunScatteringLuminanceScale, 0.0f);

    /** 同じ位相と光学的深さを使う基準照明。 */
    FVolumetricCloudLighting reference{};
    reference.SunScatter = 1.0f;
    reference.SunScatteringLuminanceScale = 1.0f;
    /** 位相関数を変えず、アルベドと輝度補償だけを変える照明。 */
    FVolumetricCloudLighting scaled = reference;
    scaled.SunScatter = 0.25f;
    scaled.SunScatteringLuminanceScale = 3.0f;
    /** 基準の一次と高次の有向散乱。 */
    const FVec2 referenceScattering = EvaluateVolumetricCloudDirectionalScattering(2.0f, 2.0f, 1.0f, reference);
    /** 同じ輸送に0.25×3.0の光源倍率だけを適用した散乱。 */
    const FVec2 scaledScattering = EvaluateVolumetricCloudDirectionalScattering(2.0f, 2.0f, 1.0f, scaled);
    EXPECT_NEAR(scaledScattering.x, referenceScattering.x * 0.75f, 1e-6f);
    EXPECT_NEAR(scaledScattering.y, referenceScattering.y * 0.75f, 1e-6f);

    /** 吸収のみの媒質は、補償倍率があっても散乱しない設定。 */
    scaled.SunScatter = 0.0f;
    scaled.SunScatteringLuminanceScale =
        kVolumetricCloudMaxSunScatteringLuminanceScale;
    /** 散乱アルベド0で消える有向散乱。 */
    const FVec2 absorbingScattering = EvaluateVolumetricCloudDirectionalScattering(2.0f, 2.0f, 1.0f, scaled);
    EXPECT_NEAR(absorbingScattering.x, 0.0f, 0.0f);
    EXPECT_NEAR(absorbingScattering.y, 0.0f, 0.0f);
}

ACS_TEST(VolumetricCloudSettings, WeatherControlsRemainBoundedAndDefaultToProceduralInput)
{
    /** 影響率 0 により、直接利用時の手続き天候場を変えない既定値。 */
    const FVolumetricCloudWeather defaults{};
    EXPECT_NEAR(defaults.CloudType, 0.78f, 0.0f);
    EXPECT_NEAR(defaults.CloudTypeInfluence, 0.0f, 0.0f);
    EXPECT_NEAR(defaults.Precipitation, 0.0f, 0.0f);
    EXPECT_NEAR(defaults.PrecipitationInfluence, 0.0f, 0.0f);

    /** 非有限値と範囲外値を含む入力。 */
    FVolumetricCloudWeather requested{};
    requested.CloudType = std::numeric_limits<f32>::quiet_NaN();
    requested.CloudTypeInfluence = 2.0f;
    requested.Precipitation = -1.0f;
    requested.PrecipitationInfluence = std::numeric_limits<f32>::infinity();
    /** GPU へ渡せる範囲へ正規化した天候。 */
    const FVolumetricCloudWeather sanitized =
        SanitizeVolumetricCloudWeather(requested);
    EXPECT_NEAR(sanitized.CloudType, defaults.CloudType, 0.0f);
    EXPECT_NEAR(sanitized.CloudTypeInfluence, 1.0f, 0.0f);
    EXPECT_NEAR(sanitized.Precipitation, 0.0f, 0.0f);
    EXPECT_NEAR(
        sanitized.PrecipitationInfluence,
        defaults.PrecipitationInfluence, 0.0f);
}

ACS_TEST(VolumetricCloudSettings, MultipleScatteringAccumulatesThroughThirdOrderAndPreservesEnergyBound)
{
    /** 消散より大きい高次散乱係数を含む入力。 */
    FVolumetricCloudLighting requested{};
    requested.MultiScatterContribution = 0.8f;
    requested.MultiScatterOcclusion = 0.4f;
    requested.SunScatter = 1.0f;
    requested.SunScatteringLuminanceScale = 1.0f;
    /** 高次散乱係数の縮小率を消散係数の縮小率以下へ直した設定。 */
    const FVolumetricCloudLighting lighting = SanitizeVolumetricCloudLighting(requested);
    EXPECT_NEAR(lighting.MultiScatterContribution, 0.4f, 0.0f);
    EXPECT_NEAR(lighting.MultiScatterOcclusion, 0.4f, 0.0f);

    /** 位相を含めた一次散乱と高次散乱の方向別係数。 */
    const FVec2 scattering = EvaluateVolumetricCloudDirectionalScattering(2.0f, 2.0f, 1.0f, lighting);
    /** 係数を一度縮小した二次散乱。 */
    const f32 expectedSecond = 0.4f * std::exp(-0.8f);
    /** 係数を二度縮小した三次散乱。 */
    const f32 expectedThird = 0.16f * std::exp(-0.32f);
    EXPECT_NEAR(scattering.x, std::exp(-2.0f) * 2.0f, 1e-6f);
    EXPECT_NEAR(scattering.y, expectedSecond + expectedThird, 1e-6f);
    EXPECT_TRUE(scattering.y > expectedSecond);
    EXPECT_TRUE(scattering.x + scattering.y > scattering.x);

    /** 高次散乱を切った単散乱のみの設定。 */
    FVolumetricCloudLighting singleOnly = lighting;
    singleOnly.MultiScatterContribution = 0.0f;
    /** 高次散乱を切っても変化しない一次散乱係数。 */
    const FVec2 withoutMultiple = EvaluateVolumetricCloudDirectionalScattering(2.0f, 2.0f, 1.0f, singleOnly);
    EXPECT_NEAR(withoutMultiple.x, scattering.x, 1e-6f);
    EXPECT_NEAR(withoutMultiple.y, 0.0f, 0.0f);

    /** 位相上限を越える入力を評価した有界な係数。 */
    const FVec2 boundedPhase = EvaluateVolumetricCloudDirectionalScattering(0.0f, 1000.0f, 1000.0f, lighting);
    EXPECT_NEAR(boundedPhase.x, lighting.PhaseMax, 0.0f);
    EXPECT_NEAR(boundedPhase.y, (0.4f + 0.16f) * lighting.PhaseMax, 1e-6f);
}

ACS_TEST(VolumetricCloudSettings, HigherOrderScatteringRequiresNeighbouringMedium)
{
    /** 既定照明で、遮蔽された地点の一次散乱と高次散乱を分離した値。 */
    const FVolumetricCloudLighting lighting{};
    const FVec2 scattering = EvaluateVolumetricCloudDirectionalScattering(
        2.0f, 0.4f, 1.0f, lighting);
    EXPECT_TRUE(scattering.x > 0.0f);
    EXPECT_TRUE(scattering.y > scattering.x);

    /** 雲頂の疎な領域で、周囲散乱源が存在する有界な確率。 */
    const f32 sparseFactor = EvaluateVolumetricCloudInScatterFactor(
        0.0f, 1.0f, lighting.PowderStrength);
    /** 密な領域では高次散乱を減らさない中立係数。 */
    const f32 denseFactor = EvaluateVolumetricCloudInScatterFactor(
        1.0f, 1.0f, lighting.PowderStrength);
    EXPECT_TRUE(sparseFactor > 0.0f && sparseFactor < 1.0f);
    EXPECT_NEAR(denseFactor, 1.0f, 0.0f);

    /** 旧順序は方向性を持つ一次散乱を減らし、等方に近い高次散乱を雲縁でも全量残していた。 */
    const f32 formerSparseTotal =
        scattering.x * sparseFactor + scattering.y;
    /** 補正後は一次散乱を保ち、周囲媒質を必要とする高次散乱だけを減らす。 */
    const f32 correctedSparseTotal =
        scattering.x + scattering.y * sparseFactor;
    EXPECT_TRUE(correctedSparseTotal < formerSparseTotal);
    EXPECT_NEAR(
        scattering.x + scattering.y * denseFactor,
        scattering.x + scattering.y, 0.0f);
}

ACS_TEST(VolumetricCloudSettings, HigherOrderScatterFactorUsesDensityWithoutDarkeningDenseCloudBase)
{
    /** 補正を切ったときの中立係数。 */
    const f32 disabled = EvaluateVolumetricCloudInScatterFactor(0.0f, 0.0f, 0.0f);
    EXPECT_NEAR(disabled, 1.0f, 0.0f);

    /** 雲頂の空に近い低密度域へ完全適用した内部散乱係数。 */
    const f32 sparseTop = EvaluateVolumetricCloudInScatterFactor(0.0f, 1.0f, 1.0f);
    EXPECT_NEAR(sparseTop, 0.05f, 1e-6f);
    /** 雲頂の密な領域は内部散乱確率が飽和する。 */
    const f32 denseTop = EvaluateVolumetricCloudInScatterFactor(1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(denseTop, 1.0f, 0.0f);
    /** 密な雲底の直上には周囲媒質があるため、高さだけでは高次散乱を減らさない。 */
    const f32 denseBase = EvaluateVolumetricCloudInScatterFactor(1.0f, 0.0f, 1.0f);
    EXPECT_NEAR(denseBase, 1.0f, 0.0f);
    EXPECT_NEAR(denseBase, denseTop, 0.0f);

    /** 同じ中密度なら雲底側ほど上方の周囲媒質を残し、疎な雲頂縁は強く抑える。 */
    const f32 middleBase = EvaluateVolumetricCloudInScatterFactor(0.25f, 0.0f, 1.0f);
    const f32 middleTop = EvaluateVolumetricCloudInScatterFactor(0.25f, 1.0f, 1.0f);
    EXPECT_NEAR(middleBase, 0.55f, 1e-6f);
    EXPECT_NEAR(middleTop, 0.1125f, 1e-6f);
    EXPECT_TRUE(middleBase > middleTop);

    /** 既定の混ぜ率でも、空に近い疎な領域の高次散乱を約0.715へ滑らかに抑える。 */
    const FVolumetricCloudLighting defaults{};
    const f32 defaultSparseBase = EvaluateVolumetricCloudInScatterFactor(0.0f, 0.0f, defaults.PowderStrength);
    EXPECT_NEAR(defaultSparseBase, 0.715f, 1e-6f);

    /** 非有限入力を含めても増幅せず有限な中立値へ戻る。 */
    const f32 hostile = EvaluateVolumetricCloudInScatterFactor(
        std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::quiet_NaN(),
        std::numeric_limits<f32>::infinity());
    EXPECT_TRUE(std::isfinite(hostile));
    EXPECT_NEAR(hostile, 1.0f, 0.0f);

    /** 公開設定の混ぜ率は確率範囲を越えない。 */
    FVolumetricCloudLighting excessive{};
    excessive.PowderStrength = 100.0f;
    EXPECT_NEAR(SanitizeVolumetricCloudLighting(excessive).PowderStrength, 1.0f, 0.0f);
}

ACS_TEST(VolumetricCloudSettings, RangeBoundsDistanceFadeGrowthAndWork)
{
    /** 非有限距離を既定値へ戻す入力。 */
    FVolumetricCloudRange nonFinite{};
    nonFinite.MaxDistance = std::numeric_limits<f32>::infinity();
    nonFinite.FadeFraction = -1.0f;
    nonFinite.StepGrowth = 100.0f;
    nonFinite.ViewSteps = 1u;
    /** 最初の境界入力を正規化した結果。 */
    const FVolumetricCloudRange first = SanitizeVolumetricCloudRange(nonFinite);
    EXPECT_NEAR(first.MaxDistance, kVolumetricCloudMaxDistance, 0.0f);
    EXPECT_NEAR(first.FadeFraction, 0.0f, 0.0f);
    EXPECT_NEAR(first.StepGrowth, kVolumetricCloudMaxStepGrowth, 0.0f);
    EXPECT_EQ(first.ViewSteps, kVolumetricCloudMinViewSteps);

    /** 最小距離と最大仕事量を越える入力。 */
    FVolumetricCloudRange extremes{};
    extremes.MaxDistance = -100.0f;
    extremes.FadeFraction = std::numeric_limits<f32>::quiet_NaN();
    extremes.StepGrowth = -2.0f;
    extremes.ViewSteps = std::numeric_limits<u32>::max();
    /** 2 番目の境界入力を正規化した結果。 */
    const FVolumetricCloudRange second = SanitizeVolumetricCloudRange(extremes);
    EXPECT_NEAR(second.MaxDistance, kVolumetricCloudMinDistance, 0.0f);
    EXPECT_NEAR(second.FadeFraction, 0.28f, 0.0f);
    EXPECT_NEAR(second.StepGrowth, 0.0f, 0.0f);
    EXPECT_EQ(second.ViewSteps, kVolumetricCloudMaxViewMarchSamples);
}

ACS_TEST(VolumetricCloudSettings, LocalViewDistanceIsContinuousInsideAndOutsideLayerBoundaries)
{
    /** 通常の積雲層と層外で使う遠景距離。 */
    constexpr f32 baseHeight = 2600.0f;
    constexpr f32 topHeight = 5200.0f;
    constexpr f32 maximumDistance = 250000.0f;
    /** 層厚の4倍から求まる中央部の局所視程。 */
    constexpr f32 interiorDistance = 10400.0f;

    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(baseHeight, baseHeight, topHeight, maximumDistance), interiorDistance, 0.01f);
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(3900.0f, baseHeight, topHeight, maximumDistance), interiorDistance, 0.01f);
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(topHeight, baseHeight, topHeight, maximumDistance), interiorDistance, 0.01f);

    /** 層外の近接帯中央では逆距離が正確に半分ずつ混ざる。 */
    const f32 transitionDistance = (topHeight - baseHeight) * kVolumetricCloudBoundaryTransitionFraction;
    const f32 expectedMidpoint = 1.0f / (0.5f / maximumDistance + 0.5f / interiorDistance);
    const f32 entering = EvaluateVolumetricCloudInteriorViewDistance(baseHeight - transitionDistance * 0.5f, baseHeight, topHeight, maximumDistance);
    const f32 leaving = EvaluateVolumetricCloudInteriorViewDistance(topHeight + transitionDistance * 0.5f, baseHeight, topHeight, maximumDistance);
    EXPECT_NEAR(entering, expectedMidpoint, 0.01f);
    EXPECT_NEAR(leaving, entering, 0.01f);
    EXPECT_TRUE(entering > interiorDistance);
    EXPECT_TRUE(entering < maximumDistance);
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(baseHeight - transitionDistance, baseHeight, topHeight, maximumDistance), maximumDistance, 0.0f);
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(topHeight + transitionDistance, baseHeight, topHeight, maximumDistance), maximumDistance, 0.0f);
    EXPECT_TRUE(EvaluateVolumetricCloudInteriorViewDistance(topHeight + 1.0f, baseHeight, topHeight, maximumDistance) < entering);
    /** 雲頂から200 mの通常上空視点は局所視程を保ち、遠上空では指定距離へ戻る。 */
    EXPECT_TRUE(EvaluateVolumetricCloudInteriorViewDistance(5400.0f, baseHeight, topHeight, maximumDistance) < 12000.0f);
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(8000.0f, baseHeight, topHeight, maximumDistance), maximumDistance, 0.0f);

    /** 薄い層と厚い層は公開した8～35 kmの範囲へ収まる。 */
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(500.0f, 0.0f, 1000.0f, maximumDistance), kVolumetricCloudInteriorMinDistance, 0.01f);
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(5000.0f, 0.0f, 10000.0f, maximumDistance), kVolumetricCloudInteriorMaxDistance, 0.01f);
    /** 利用側が局所視程より短く指定した場合は、その指定を広げない。 */
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(3900.0f, baseHeight, topHeight, 6000.0f), 6000.0f, 0.0f);
    /** 不正な高度または層は正規化した遠景距離へ戻る。 */
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(std::numeric_limits<f32>::quiet_NaN(), baseHeight, topHeight, maximumDistance), maximumDistance, 0.0f);
    EXPECT_NEAR(EvaluateVolumetricCloudInteriorViewDistance(3900.0f, topHeight, baseHeight, maximumDistance), maximumDistance, 0.0f);
}

ACS_TEST(VolumetricCloudSettings, UpperLayerCannotIntersectLowerDensityBand)
{
    /** 上下逆転と小さすぎるノイズ尺度を含む下層入力。 */
    const FVolumetricCloudLayer lower = SanitizeVolumetricCloudLayer(FVolumetricCloudLayer{20.0f, 10.0f, 0.0f});
    EXPECT_NEAR(lower.base_height, 10.0f, 0.0f);
    EXPECT_NEAR(lower.top_height, 20.0f, 0.0f);
    EXPECT_NEAR(lower.horizontal_noise_scale, 0.001f, 0.0f);

    /** 下層へ食い込むが上端は成立する上層入力。 */
    const FVolumetricCloudUpperLayer clamped =
        SanitizeVolumetricCloudUpperLayer(FVolumetricCloudUpperLayer{15.0f, 30.0f, 2.0f, -1.0f}, lower);
    EXPECT_NEAR(clamped.base_height, lower.top_height, 0.0f);
    EXPECT_NEAR(clamped.top_height, 30.0f, 0.0f);
    EXPECT_NEAR(clamped.coverage_scale, 1.0f, 0.0f);
    EXPECT_NEAR(clamped.density_scale, 0.0f, 0.0f);

    /** 下層より上へ出ないため無効化される上層入力。 */
    const FVolumetricCloudUpperLayer disabled =
        SanitizeVolumetricCloudUpperLayer(FVolumetricCloudUpperLayer{12.0f, 19.0f, 0.5f, 0.3f}, lower);
    EXPECT_NEAR(disabled.base_height, 0.0f, 0.0f);
    EXPECT_NEAR(disabled.top_height, 0.0f, 0.0f);

    /** 下層変更時の上層再検査を確認する CPU 所有オブジェクト。 */
    CVolumetricClouds clouds;
    clouds.SetLayer(FVolumetricCloudLayer{100.0f, 200.0f, 0.035f});
    clouds.SetUpperLayer(FVolumetricCloudUpperLayer{300.0f, 400.0f, 0.5f, 0.3f});
    EXPECT_NEAR(clouds.UpperLayer().base_height, 300.0f, 0.0f);
    clouds.SetLayer(FVolumetricCloudLayer{350.0f, 450.0f, 0.035f});
    EXPECT_NEAR(clouds.UpperLayer().base_height, 0.0f, 0.0f);
    EXPECT_NEAR(clouds.UpperLayer().top_height, 0.0f, 0.0f);
}

ACS_TEST(VolumetricCloudSettings, PublicSettersStoreTheSameSanitizedValuesUsedByGpuUpload)
{
    /** 各 setter の境界外入力を保持する雲描画。 */
    CVolumetricClouds clouds;
    /** 負の消散を含む照明入力。 */
    FVolumetricCloudLighting lighting{};
    lighting.ViewExtinction = -1.0f;
    lighting.SunTransmittance = FVec3{2.0f, -1.0f, 0.5f};
    lighting.SunScatteringLuminanceScale = 1000.0f;
    clouds.SetLighting(lighting);
    EXPECT_NEAR(clouds.Lighting().ViewExtinction, 0.0f, 0.0f);
    EXPECT_NEAR(clouds.Lighting().SunTransmittance.x, 1.0f, 0.0f);
    EXPECT_NEAR(clouds.Lighting().SunTransmittance.y, 0.0f, 0.0f);
    EXPECT_NEAR(clouds.Lighting().SunTransmittance.z, 0.5f, 0.0f);
    EXPECT_NEAR(clouds.Lighting().SunScatteringLuminanceScale, kVolumetricCloudMaxSunScatteringLuminanceScale, 0.0f);

    /** 範囲外の雲種と降水成分を含む天候入力。 */
    FVolumetricCloudWeather weather{};
    weather.CloudType = -1.0f;
    weather.CloudTypeInfluence = 2.0f;
    weather.Precipitation = 2.0f;
    weather.PrecipitationInfluence = -1.0f;
    clouds.SetWeather(weather);
    EXPECT_NEAR(clouds.Weather().CloudType, 0.0f, 0.0f);
    EXPECT_NEAR(clouds.Weather().CloudTypeInfluence, 1.0f, 0.0f);
    EXPECT_NEAR(clouds.Weather().Precipitation, 1.0f, 0.0f);
    EXPECT_NEAR(clouds.Weather().PrecipitationInfluence, 0.0f, 0.0f);

    /** 過大な距離と刻み数を含む範囲入力。 */
    FVolumetricCloudRange range{};
    range.MaxDistance = 1.0e30f;
    range.ViewSteps = 10000u;
    clouds.SetRange(range);
    EXPECT_NEAR(clouds.Range().MaxDistance, kVolumetricCloudMaxDistance, 0.0f);
    EXPECT_EQ(clouds.Range().ViewSteps, kVolumetricCloudMaxViewMarchSamples);

    clouds.SetReferenceMode(true);
    EXPECT_TRUE(clouds.ReferenceMode());
    clouds.SetReferenceMode(false);
    EXPECT_FALSE(clouds.ReferenceMode());
}

ACS_TEST(VolumetricCloudSettings,
         EnvironmentSignatureTracksSettingsWithoutPerFrameTime)
{
    CVolumetricClouds clouds;
    const u32 original = clouds.EnvironmentLightingSignature(
        0.55f, 1.6f, 1.0f);
    EXPECT_TRUE(original != 0u);
    EXPECT_EQ(
        clouds.EnvironmentLightingSignature(0.55f, 1.6f, 1.0f),
        original);
    // 連続補間の隣接値は同じ量子化区間に収まり、微小差だけでは無効化しない。
    EXPECT_EQ(clouds.EnvironmentLightingSignature(0.551f, 1.61f, 1.02f), original);

    // 画面用の参照品質は環境の物理状態ではないため署名を変えない。
    clouds.SetReferenceMode(true);
    EXPECT_EQ(
        clouds.EnvironmentLightingSignature(0.55f, 1.6f, 1.0f),
        original);

    // 見た目へ届く大きさのcoverage、密度、風速変更は署名を変える。
    EXPECT_TRUE(
        clouds.EnvironmentLightingSignature(0.60f, 1.6f, 1.0f)
        != original);
    EXPECT_TRUE(
        clouds.EnvironmentLightingSignature(0.55f, 1.8f, 1.0f)
        != original);
    EXPECT_TRUE(
        clouds.EnvironmentLightingSignature(0.55f, 1.6f, 2.0f)
        != original);

    FVolumetricCloudLighting lighting = clouds.Lighting();
    lighting.AmbientAtTop += 0.1f;
    clouds.SetLighting(lighting);
    const u32 lighting_changed = clouds.EnvironmentLightingSignature(
        0.55f, 1.6f, 1.0f);
    EXPECT_TRUE(lighting_changed != original);

    // 有向散乱の輝度補償も環境cubemapの実際の照明入力である。
    lighting = clouds.Lighting();
    lighting.SunScatteringLuminanceScale += 0.5f;
    clouds.SetLighting(lighting);
    const u32 compensation_changed = clouds.EnvironmentLightingSignature(0.55f, 1.6f, 1.0f);
    EXPECT_TRUE(compensation_changed != lighting_changed);

    // Sceneが毎frame上書きする大気色も、環境cubemapの実際の照明入力なので追跡する。
    // 再生成頻度は固定frame間隔で別に制限される。
    lighting = clouds.Lighting();
    lighting.SkyZenithColor = FVec3{4.0f, 3.0f, 2.0f};
    lighting.SunTransmittance = FVec3{0.4f, 0.5f, 0.6f};
    clouds.SetLighting(lighting);
    EXPECT_TRUE(clouds.EnvironmentLightingSignature(0.55f, 1.6f, 1.0f) != compensation_changed);

    clouds.SetLayer(FVolumetricCloudLayer{1800.0f, 4300.0f, 0.035f});
    EXPECT_TRUE(
        clouds.EnvironmentLightingSignature(0.55f, 1.6f, 1.0f)
        != lighting_changed);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const u32 hostile = clouds.EnvironmentLightingSignature(
        nan, nan, nan);
    EXPECT_TRUE(hostile != 0u);
    EXPECT_EQ(
        clouds.EnvironmentLightingSignature(nan, nan, nan),
        hostile);
}

ACS_TEST(VolumetricCloudSettings, EnvironmentLightingRefreshCadenceIsBoundedAndEventual)
{
    CVolumetricClouds clouds;
    EXPECT_EQ(clouds.RenderedEnvironmentLightingUpdateSignature(), 0u);
    const u32 firstGeneration = clouds.EnvironmentLightingUpdateSignature(0.55f, 1.6f, 1.0f, 1u);
    EXPECT_TRUE(firstGeneration != 0u);
    EXPECT_EQ(clouds.EnvironmentLightingUpdateSignature(0.55f, 1.6f, 1.0f, 29u), firstGeneration);
    const u32 secondGeneration = clouds.EnvironmentLightingUpdateSignature(0.55f, 1.6f, 1.0f, 30u);
    EXPECT_TRUE(secondGeneration != firstGeneration);
    EXPECT_EQ(clouds.EnvironmentLightingUpdateSignature(0.55f, 1.6f, 1.0f, 31u), secondGeneration);
    EXPECT_TRUE(clouds.EnvironmentLightingUpdateSignature(0.55f, 1.6f, 1.0f, 60u) != secondGeneration);
    EXPECT_EQ(clouds.EnvironmentLightingUpdateSignature(0.55f, 1.6f, 1.0f, 30u), secondGeneration);
    EXPECT_TRUE(clouds.EnvironmentLightingUpdateSignature(0.55f, 1.6f, 1.0f, ~u64{0}) != 0u);

    EXPECT_FALSE(CVolumetricClouds::IsEnvironmentLightingRefreshFrame(0u));
    for (u64 frame = 1u; frame < 30u; ++frame) {
        EXPECT_FALSE(CVolumetricClouds::IsEnvironmentLightingRefreshFrame(frame));
    }
    EXPECT_TRUE(CVolumetricClouds::IsEnvironmentLightingRefreshFrame(30u));
    EXPECT_FALSE(CVolumetricClouds::IsEnvironmentLightingRefreshFrame(31u));
    EXPECT_TRUE(CVolumetricClouds::IsEnvironmentLightingRefreshFrame(60u));
}

ACS_TEST(VolumetricCloudSettings,
         EnvironmentBakeReusesVolumeAndPublishesDerivedMapsTransactionally)
{
    const std::string sky_source = ReadRenderFile("Sky.cpp");
    const std::string ibl_source = ReadRenderFile("Ibl.cpp");
    EXPECT_TRUE(!sky_source.empty());
    EXPECT_TRUE(!ibl_source.empty());

    const auto signature = SliceBetween(sky_source, "u32 CVolumetricClouds::EnvironmentLightingSignature(", "u32 CVolumetricClouds::EnvironmentLightingUpdateSignature(");
    EXPECT_TRUE(Contains(signature, "m_PrevSunColor.x"));
    EXPECT_TRUE(Contains(signature, "m_PrevSkyColor.x"));
    EXPECT_TRUE(Contains(signature, "m_Lighting.SunTransmittance.x"));
    EXPECT_TRUE(Contains(signature, "m_Lighting.SkyZenithColor.x"));
    EXPECT_TRUE(Contains(signature, "HashCloudEnvironmentQuantizedFloat("));
    EXPECT_TRUE(Contains(signature, "ResolveVolumetricCloudViewDistance_Internal("));
    EXPECT_TRUE(Contains(signature, "kCloudEnvironmentViewDistanceSignatureStep"));

    const auto update_signature = SliceBetween(sky_source, "u32 CVolumetricClouds::EnvironmentLightingUpdateSignature(", "u32 CVolumetricClouds::RenderedEnvironmentLightingUpdateSignature(");
    EXPECT_TRUE(Contains(update_signature, "submission_index / kVolumetricCloudEnvironmentRefreshInterval"));
    EXPECT_TRUE(Contains(update_signature, "HashCloudEnvironmentWord(hash, static_cast<u32>(updateGeneration))"));
    EXPECT_TRUE(Contains(update_signature, "HashCloudEnvironmentWord(hash, static_cast<u32>(updateGeneration >> 32u))"));

    const auto rendered_signature = SliceBetween(sky_source, "u32 CVolumetricClouds::RenderedEnvironmentLightingUpdateSignature(", "bool CVolumetricClouds::IsEnvironmentLightingRefreshFrame(");
    EXPECT_TRUE(Contains(rendered_signature, "!m_LastFrameWorkload.submitted"));
    EXPECT_TRUE(Contains(rendered_signature, "m_PrevCoverage, m_PrevDensity, m_PrevWindSpeed"));
    EXPECT_TRUE(Contains(rendered_signature, "m_LastFrameWorkload.submission_index"));

    const std::string environment = SliceBetween(
        sky_source,
        "CVolumetricClouds::BuildEnvironmentCubemap(",
        "FVolumetricCloudWorldShadowMap");
    EXPECT_TRUE(Contains(environment, "cl.SetComputePipeline(*m_CloudPipe);"));
    EXPECT_TRUE(Contains(environment, "cl.SetTexture(0, *m_ShapeTex);"));
    EXPECT_TRUE(Contains(environment, "cl.SetTexture(1, *m_WeatherTex);"));
    EXPECT_TRUE(Contains(environment, "cl.SetTexture(2, *m_DetailTex);"));
    EXPECT_TRUE(Contains(environment, "cl.SetTexture(3, *m_CurlTex);"));
    EXPECT_TRUE(Contains(environment, "m_Lighting.SunScatter"));
    EXPECT_TRUE(Contains(environment, "m_Lighting.MultiScatterContribution"));
    EXPECT_TRUE(Contains(environment, "base_environment"));
    EXPECT_TRUE(Contains(environment, "for (u32 face = 0u; face < 6u; ++face)"));
    EXPECT_TRUE(Contains(environment, "TUniquePtr<IRhiBuffer> cloud_cb[6]"));
    EXPECT_TRUE(Contains(environment, "TUniquePtr<IRhiBuffer> composite_cb[6]"));
    EXPECT_TRUE(Contains(environment, "cl.SetConstantBuffer(0, *cloud_cb[face])"));
    EXPECT_FALSE(Contains(environment, "ReadTexture"));
    EXPECT_FALSE(Contains(environment, "m_HistoryColor["));
    EXPECT_FALSE(Contains(environment, "m_HistoryDepth["));
    EXPECT_FALSE(Contains(environment, "m_Cb->Update"));

    const std::string rebuild = SliceBetween(
        ibl_source,
        "CImageBasedLighting::RebuildDerivedMapsFromEnvironment(",
        "CImageBasedLighting::EnsureSkyboxPipeline(");
    const std::size_t irradiance_build = rebuild.find(
        "BuildIrradiance(");
    const std::size_t prefilter_build = rebuild.find(
        "BuildPrefilter(", irradiance_build);
    const std::size_t irradiance_publish = rebuild.find(
        "m_IrradianceCube = Move(irradiance_candidate);",
        prefilter_build);
    const std::size_t prefilter_publish = rebuild.find(
        "m_PrefilterCube = Move(prefilter_candidate);",
        irradiance_publish);
    EXPECT_TRUE(irradiance_build != std::string::npos);
    EXPECT_TRUE(prefilter_build != std::string::npos);
    EXPECT_TRUE(irradiance_publish != std::string::npos);
    EXPECT_TRUE(prefilter_publish != std::string::npos);
    EXPECT_TRUE(irradiance_build < prefilter_build);
    EXPECT_TRUE(prefilter_build < irradiance_publish);
    EXPECT_TRUE(irradiance_publish < prefilter_publish);
    EXPECT_FALSE(Contains(rebuild, "WaitIdle("));
}

ACS_TEST(VolumetricCloudSettings, EffectiveChangesInvalidateOnlyDependentCaches)
{
    /** CPU の設定処理と GPU シェーダを含む実装。 */
    const std::string source = ReadRenderFile("Sky.cpp");
    /** 公開 adapter と private 内部処理を含む宣言。 */
    const std::string header = ReadRenderFile("Sky.h");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!header.empty());

    /** 下層 setter の実装範囲。 */
    const std::string setLayer = SliceBetween(source, "void CVolumetricClouds::SetLayer(",
                                              "void CVolumetricClouds::SetReferenceMode(");
    /** 参照モード setter の実装範囲。 */
    const std::string setReference = SliceBetween(source, "void CVolumetricClouds::SetReferenceMode(",
                                                  "void CVolumetricClouds::SetLighting(");
    /** 照明 setter の実装範囲。 */
    const std::string setLighting = SliceBetween(source, "void CVolumetricClouds::SetLighting(",
                                                 "void CVolumetricClouds::SetWeather(");
    /** 天候 setter の実装範囲。 */
    const std::string setWeather = SliceBetween(source, "void CVolumetricClouds::SetWeather(",
                                                "void CVolumetricClouds::SetRange(");
    /** 距離 setter の実装範囲。 */
    const std::string setRange = SliceBetween(source, "void CVolumetricClouds::SetRange(",
                                              "void CVolumetricClouds::SetUpperLayer(");
    /** 上層 setter の実装範囲。 */
    const std::string setUpper = SliceBetween(source, "void CVolumetricClouds::SetUpperLayer(",
                                              "EShaderStatus CVolumetricClouds::FCompiledShaders::Status(");
    /** 最終被覆から追加採取無しで求める低 LOD 密度の実装範囲。 */
    const std::string lowLodDensity = SliceBetween(source, "float cloudLowLodDensityFromPositiveWeatherMacro(",
                                                   "float cloudDensityFromPositiveWeatherMacro(");
    EXPECT_TRUE(Contains(setLayer, "InvalidateCloudHistory_Internal(true);"));
    EXPECT_TRUE(Contains(setUpper, "InvalidateCloudHistory_Internal(true);"));
    EXPECT_TRUE(Contains(setReference, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(setLighting, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(setWeather, "InvalidateCloudHistory_Internal(true);"));
    EXPECT_TRUE(Contains(setRange, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(header, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(source, "if (density_field_changed) m_ShadowCacheValid = false;"));
    EXPECT_TRUE(Contains(lowLodDensity, "cloudDimensionalProfile("));
    EXPECT_TRUE(Contains(lowLodDensity, "macro.heightProfile,weatherMask"));
    EXPECT_TRUE(Contains(lowLodDensity, "cloudNormalizedBaseDensity("));
    EXPECT_FALSE(Contains(lowLodDensity, "result.y="));
    EXPECT_FALSE(Contains(lowLodDensity, "cloudDimensionalDensity("));
    EXPECT_TRUE(Contains(lowLodDensity, "cloudDensityFromDimensionalProfile("));
    EXPECT_TRUE(Contains(lowLodDensity, "dimensionalDensity"));
    EXPECT_TRUE(Contains(lowLodDensity, "cloudHeightPrecipitationDensityScale("));
    EXPECT_FALSE(Contains(lowLodDensity, "baseDensity*weatherMask*macro.heightProfile"));
    EXPECT_TRUE(Contains(source, "if(upperBand) weatherMask*=cloudUpperTerms.x;"));
    EXPECT_TRUE(Contains(source, "if(upperBand) densityResult*=cloudUpperTerms.y;"));
    EXPECT_FALSE(Contains(lowLodDensity, "SampleLevel"));

    EXPECT_TRUE(Contains(source, "cached.y"));
    EXPECT_FALSE(Contains(source, "cached.y*density*cloudLightingExtinction.y"));
    EXPECT_TRUE(Contains(source, "float tauL=lightDepth*density*cloudLightingExtinction.y;"));
    EXPECT_TRUE(Contains(source, "*lightTerminationOcclusion>18.0"));
    EXPECT_FALSE(Contains(source, "cloudForwardPhaseWeight"));
    EXPECT_TRUE(Contains(source, "float cloudReducedIntervalScatteringWeight("));
    EXPECT_TRUE(Contains(source, "float cosA=clamp(dot(-dir,sun),-1.0,1.0);"));
    EXPECT_FALSE(Contains(source, "float cosA=clamp(dot(dir,sun),-1.0,1.0);"));
    EXPECT_TRUE(Contains(source, "float forwardPhase=hg(cosA,cloudLightingPhase.x);"));
    EXPECT_TRUE(Contains(source, "float backwardPhase=hg(cosA,cloudLightingPhase.y);"));
    EXPECT_FALSE(Contains(source, "4.0*hg("));
    EXPECT_TRUE(Contains(source, "float phase=lerp("));
    EXPECT_TRUE(Contains(source, "backwardPhase,forwardPhase,saturate(phaseBlend)"));
    EXPECT_TRUE(Contains(source, "float inScatterDepthExponent=lerp("));
    EXPECT_TRUE(Contains(source, "float lowLodDensity=cloudLowLodDensityFromMacro("));
    EXPECT_FALSE(Contains(source, "lowLodDensityAndProfile"));
    EXPECT_TRUE(Contains(source, "0.05+pow(saturate(lowLodDensity),inScatterDepthExponent)"));
    EXPECT_TRUE(Contains(source, "float inScatterProbability=inScatterDepth;"));
    EXPECT_FALSE(Contains(source, "inScatterVertical"));
    EXPECT_TRUE(Contains(source, "1.0,inScatterProbability,cloudLightingExtinction.w"));
    EXPECT_TRUE(Contains(source, "float thirdContribution=multiContribution*multiContribution;"));
    EXPECT_TRUE(Contains(source, "float thirdOcclusion=multiOcclusion*multiOcclusion;"));
    EXPECT_TRUE(Contains(source, "float secondLightTransmittance=exp(-tauL*multiOcclusion);"));
    EXPECT_TRUE(Contains(source, "float thirdLightTransmittance=exp(-tauL*thirdOcclusion);"));
    EXPECT_TRUE(Contains(source, "float directionalScatteringScale=cloudLightingExtinction.z"));
    EXPECT_TRUE(Contains(source, "*cloudLightingGround.w;"));
    EXPECT_TRUE(Contains(source, "float3 singleSunL=sunAtCloud*directionalScatteringScale"));
    EXPECT_TRUE(Contains(source, "float3 secondSunL=sunAtCloud*directionalScatteringScale"));
    EXPECT_TRUE(Contains(source, "float3 thirdSunL=sunAtCloud*directionalScatteringScale"));
    EXPECT_TRUE(Contains(source, "float secondSampleWeight=secondOrderTransmit"));
    EXPECT_TRUE(Contains(source, "float thirdSampleWeight=thirdOrderTransmit"));
    EXPECT_TRUE(Contains(source, "+secondSampleWeight*secondSunL"));
    EXPECT_TRUE(Contains(source, "+thirdSampleWeight*thirdSunL;"));
    EXPECT_TRUE(Contains(source, "secondOrderTransmit*=secondIntervalTransmittance;"));
    EXPECT_TRUE(Contains(source, "thirdOrderTransmit*=thirdIntervalTransmittance;"));
    EXPECT_TRUE(Contains(source, "float h=macro.height;"));
    EXPECT_FALSE(Contains(source, "ambientSurfaceProbability"));
    EXPECT_FALSE(Contains(source, "topSurfaceScatter"));
    EXPECT_FALSE(Contains(source, "float singleScatter="));
    EXPECT_FALSE(Contains(source, "float multipleScatter="));
    EXPECT_FALSE(Contains(source, "nearLightDensity"));
    EXPECT_FALSE(Contains(source, "edgeBoost"));
    EXPECT_FALSE(Contains(source, "1.0-exp(-dens*cloudLightingExtinction.w)"));
    EXPECT_FALSE(Contains(source, "pow(saturate(shape),inScatterDepthExponent)"));
    EXPECT_FALSE(Contains(source, "beer*(1.0-multiWeight)*phase"));
    EXPECT_FALSE(Contains(source, "hg(cosA,cloudLightingPhase.x)*phaseBlend"));
    EXPECT_FALSE(Contains(source, "density*4.2"));
}

ACS_TEST(VolumetricCloudSettings, AmbientVisibilityUsesCachedColumnDepthWithoutDoubleAttenuation)
{
    /** GPU シェーダーを含む雲描画の実装。 */
    const std::string source = ReadRenderFile("Sky.cpp");
    /** 影キャッシュの上下積算密度から空と地面の可視率を求める範囲。 */
    const std::string ambientBlock = SliceBetween(
        source, "float ambientDensityScale=", "float intervalOpacity=1.0-intervalTransmittance;");
    EXPECT_TRUE(!ambientBlock.empty());
    EXPECT_TRUE(Contains(ambientBlock, "float ambientDensityScale=max(density*distanceFade,0.0);"));
    EXPECT_TRUE(Contains(ambientBlock, "lowLodDensity*ambientDensityScale"));
    EXPECT_FALSE(Contains(ambientBlock, "float ambientLocalDensity=saturate(lowLodDensity*density);"));
    EXPECT_TRUE(Contains(ambientBlock, "float diffuseOcclusion=multiOcclusion*multiOcclusion;"));
    EXPECT_TRUE(Contains(ambientBlock, "float reducedAmbientExtinction=0.60*diffuseOcclusion"));
    EXPECT_TRUE(Contains(ambientBlock, "*cloudLightingExtinction.y;"));
    EXPECT_TRUE(Contains(source, "float2 cloudAmbientFallbackOpticalDepth(CloudMacroSample macro,float localDensity)"));
    EXPECT_TRUE(Contains(source, "float columnThickness=max(bandThickness,0.0)"));
    EXPECT_TRUE(Contains(source, "*max(macro.columnSpan,0.0);"));
    EXPECT_TRUE(Contains(source, "*cloudOpticalDepthScaleFromBand(upperBand);"));
    EXPECT_TRUE(Contains(ambientBlock, "float2 fallbackAmbientDepth="));
    EXPECT_TRUE(Contains(ambientBlock, "cloudAmbientFallbackOpticalDepth(macro,ambientLocalDensity);"));
    EXPECT_TRUE(Contains(ambientBlock, "float fallbackSkyAmbientDepth=fallbackAmbientDepth.x;"));
    EXPECT_TRUE(Contains(ambientBlock, "float fallbackGroundAmbientDepth=fallbackAmbientDepth.y;"));
    EXPECT_FALSE(Contains(ambientBlock, "0.35+0.65"));
    EXPECT_TRUE(Contains(ambientBlock, "float3 cachedAmbientDepth=sampleCloudAmbientDepth(p);"));
    EXPECT_TRUE(Contains(ambientBlock, "cachedAmbientDepth.y*ambientDensityScale"));
    EXPECT_TRUE(Contains(ambientBlock, "cachedAmbientDepth.z*ambientDensityScale"));
    EXPECT_FALSE(Contains(source, "ambientSurfaceProbability"));
    EXPECT_FALSE(Contains(source, "lowLodDensityAndProfile"));
    EXPECT_FALSE(Contains(ambientBlock, "lowLodDensity*distanceFade"));
    EXPECT_TRUE(Contains(ambientBlock, "float skyAmbientVisibility="));
    EXPECT_TRUE(Contains(ambientBlock, "exp(-reducedAmbientExtinction*skyAmbientOpticalDepth);"));
    EXPECT_TRUE(Contains(ambientBlock, "float groundAmbientVisibility="));
    EXPECT_TRUE(Contains(ambientBlock, "exp(-reducedAmbientExtinction*groundAmbientOpticalDepth);"));
    EXPECT_TRUE(Contains(ambientBlock, "float skyAmbientZenithWeight=lerp("));
    EXPECT_TRUE(Contains(ambientBlock, "0.3333333,0.6666667,saturate(h)"));
    EXPECT_TRUE(Contains(ambientBlock, "skyCol.rgb,cloudSkyZenith.rgb,"));
    EXPECT_TRUE(Contains(ambientBlock, "skyAmbientZenithWeight"));
    EXPECT_TRUE(Contains(ambientBlock, "*skyAmbientVisibility*cloudLightingExtinction.z;"));
    EXPECT_TRUE(Contains(ambientBlock, "*bottomWeight*groundAmbientVisibility"));
    EXPECT_TRUE(Contains(ambientBlock, "*cloudLightingExtinction.z;"));
    EXPECT_FALSE(Contains(ambientBlock, "tauL"));
    EXPECT_FALSE(Contains(ambientBlock, "sun.y"));
    EXPECT_FALSE(Contains(ambientBlock, "transmit"));
    EXPECT_FALSE(Contains(source, "float viewDepth=1.0-transmit;"));
    EXPECT_FALSE(Contains(source, "float ambientOcclusion="));

    /** 0 から 1 へ制限するシェーダー側 saturate の対応式。 */
    const auto saturate = [](f32 value) noexcept {
        if (value < 0.0f) return 0.0f;
        if (value > 1.0f) return 1.0f;
        return value;
    };
    /** 低 LOD 密度とキャッシュの積算密度から入射側の可視率を求める対応式。 */
    const auto visibility = [saturate](f32 lowLodDensity, f32 density, f32 distanceFade, f32 cachedDepth, f32 lightExtinction, f32 multiScatterOcclusion) noexcept {
        const f32 safeFade = saturate(distanceFade);
        const f32 ambientDensityScale = density * safeFade > 0.0f
            ? density * safeFade : 0.0f;
        const f32 scaledDensity = lowLodDensity * ambientDensityScale;
        const f32 localDensity = scaledDensity > 0.0f ? scaledDensity : 0.0f;
        /** 層中央から局所雲柱上端までの均質媒質を積分した代替光学的厚さ。 */
        constexpr f32 kFallbackBoundaryFraction = 0.50f;
        constexpr f32 kFallbackBandThickness = 2500.0f;
        constexpr f32 kFallbackColumnSpan = 0.80f;
        const f32 fallbackDepth = localDensity * kFallbackBandThickness * kFallbackColumnSpan * kFallbackBoundaryFraction * kVolumetricCloudReferenceExtinctionPerMeter;
        const f32 opticalDepth = cachedDepth >= 0.0f
            ? cachedDepth * ambientDensityScale : fallbackDepth;
        /** 三次散乱と同じ縮小率を使う拡散光用の消散率。 */
        const f32 diffuseOcclusion = multiScatterOcclusion * multiScatterOcclusion;
        const f32 reducedExtinction = 0.60f * diffuseOcclusion * lightExtinction;
        return std::exp(-reducedExtinction * opticalDepth);
    };

    for (u32 densityStep = 0u; densityStep <= 4u; ++densityStep) {
        /** 検査対象の局所密度。 */
        const f32 density = static_cast<f32>(densityStep) * 0.25f;
        for (u32 depthStep = 0u; depthStep <= 100u; ++depthStep) {
            const f32 cachedDepth = static_cast<f32>(depthStep) * 0.016f;
            const f32 result = visibility(density, 1.0f, 1.0f, cachedDepth, 5.0f, 0.28f);
            EXPECT_TRUE(std::isfinite(result));
            EXPECT_TRUE(result >= 0.0f && result <= 1.0f);
        }
    }

    EXPECT_NEAR(visibility(0.0f, 1.0f, 1.0f, 0.0f, 5.0f, 0.28f), 1.0f, 0.0f);
    EXPECT_TRUE(std::isfinite(visibility(0.75f, 1.0f, 1.0f, -1.0f, 5.0f, 0.28f)));
    EXPECT_TRUE(visibility(0.75f, 1.0f, 1.0f, 1.2f, 5.0f, 0.28f) < visibility(0.75f, 1.0f, 1.0f, 0.2f, 5.0f, 0.28f));
    EXPECT_TRUE(visibility(0.75f, 2.0f, 1.0f, 0.8f, 5.0f, 0.28f) < visibility(0.75f, 1.0f, 1.0f, 0.8f, 5.0f, 0.28f));
    EXPECT_NEAR(visibility(0.75f, 2.0f, 0.0f, 1.6f, 5.0f, 0.28f), 1.0f, 1.0e-6f);
    // キャッシュ済み光路を使う場合、同じ点の局所密度を再度掛けても光路は増えない。
    // 旧式は局所密度だけで可視率を変え、同じ媒質を二重に減衰させていた。
    const f32 sparseSurface = visibility(
        0.10f, 1.0f, 1.0f, 0.65f, 5.0f, 0.28f);
    const f32 denseSurface = visibility(
        0.90f, 1.0f, 1.0f, 0.65f, 5.0f, 0.28f);
    EXPECT_NEAR(sparseSurface, denseSurface, 0.0f);

    /** 二点半球近似は雲底でも天頂色を、雲頂でも地平色を残す。 */
    const auto zenithWeight = [saturate](f32 height) noexcept {
        return 0.3333333f + (0.6666667f - 0.3333333f) * saturate(height);
    };
    EXPECT_NEAR(zenithWeight(0.0f), 0.3333333f, 1.0e-6f);
    EXPECT_NEAR(zenithWeight(1.0f), 0.6666667f, 1.0e-6f);
    EXPECT_NEAR(zenithWeight(0.5f), 0.5f, 1.0e-6f);
    EXPECT_TRUE(zenithWeight(-1.0f) >= 0.0f && zenithWeight(2.0f) <= 1.0f);
}
