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

ACS_TEST(VolumetricCloudSettings, MultipleScatteringAddsToSingleAndPreservesEnergyBound)
{
    /** 消散より大きい二次散乱係数を含む入力。 */
    FVolumetricCloudLighting requested{};
    requested.MultiScatterContribution = 0.8f;
    requested.MultiScatterOcclusion = 0.4f;
    /** 二次散乱係数を消散係数以下へ直した設定。 */
    const FVolumetricCloudLighting lighting = SanitizeVolumetricCloudLighting(requested);
    EXPECT_NEAR(lighting.MultiScatterContribution, 0.4f, 0.0f);
    EXPECT_NEAR(lighting.MultiScatterOcclusion, 0.4f, 0.0f);

    /** 位相を含めた一次散乱と二次散乱の方向別係数。 */
    const FVec2 scattering = EvaluateVolumetricCloudDirectionalScattering(2.0f, 2.0f, 1.0f, lighting);
    EXPECT_NEAR(scattering.x, std::exp(-2.0f) * 2.0f, 1e-6f);
    EXPECT_NEAR(scattering.y, 0.4f * std::exp(-0.8f), 1e-6f);
    EXPECT_TRUE(scattering.x + scattering.y > scattering.x);

    /** 二次散乱を切った単散乱のみの設定。 */
    FVolumetricCloudLighting singleOnly = lighting;
    singleOnly.MultiScatterContribution = 0.0f;
    /** 二次散乱を切っても変化しない一次散乱係数。 */
    const FVec2 withoutMultiple = EvaluateVolumetricCloudDirectionalScattering(2.0f, 2.0f, 1.0f, singleOnly);
    EXPECT_NEAR(withoutMultiple.x, scattering.x, 1e-6f);
    EXPECT_NEAR(withoutMultiple.y, 0.0f, 0.0f);

    /** 位相上限を越える入力を評価した有界な係数。 */
    const FVec2 boundedPhase = EvaluateVolumetricCloudDirectionalScattering(0.0f, 1000.0f, 1000.0f, lighting);
    EXPECT_NEAR(boundedPhase.x, lighting.PhaseMax, 0.0f);
    EXPECT_NEAR(boundedPhase.y, lighting.MultiScatterContribution * lighting.PhaseMax, 1e-6f);
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
    clouds.SetLighting(lighting);
    EXPECT_NEAR(clouds.Lighting().ViewExtinction, 0.0f, 0.0f);
    EXPECT_NEAR(clouds.Lighting().SunTransmittance.x, 1.0f, 0.0f);
    EXPECT_NEAR(clouds.Lighting().SunTransmittance.y, 0.0f, 0.0f);
    EXPECT_NEAR(clouds.Lighting().SunTransmittance.z, 0.5f, 0.0f);

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
                                                 "void CVolumetricClouds::SetRange(");
    /** 距離 setter の実装範囲。 */
    const std::string setRange = SliceBetween(source, "void CVolumetricClouds::SetRange(",
                                              "void CVolumetricClouds::SetUpperLayer(");
    /** 上層 setter の実装範囲。 */
    const std::string setUpper = SliceBetween(source, "void CVolumetricClouds::SetUpperLayer(",
                                              "EShaderStatus CVolumetricClouds::FCompiledShaders::Status(");
    EXPECT_TRUE(Contains(setLayer, "InvalidateCloudHistory_Internal(true);"));
    EXPECT_TRUE(Contains(setUpper, "InvalidateCloudHistory_Internal(true);"));
    EXPECT_TRUE(Contains(setReference, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(setLighting, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(setRange, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(header, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(source, "if (density_field_changed) m_ShadowCacheValid = false;"));

    EXPECT_TRUE(Contains(source, "cached.y*density*cloudLightingExtinction.y"));
    EXPECT_TRUE(Contains(source, "lightDepth*density*cloudLightingExtinction.y>18.0"));
    EXPECT_TRUE(Contains(source, "float singleScatter=beer*phase;"));
    EXPECT_TRUE(Contains(source, "float multipleScatter="));
    EXPECT_TRUE(Contains(source, "multiContribution*multi*phaseMulti;"));
    EXPECT_TRUE(Contains(source, "float scatterTerm=singleScatter+multipleScatter;"));
    EXPECT_FALSE(Contains(source, "beer*(1.0-multiWeight)*phase"));
    EXPECT_FALSE(Contains(source, "density*4.2"));
}
