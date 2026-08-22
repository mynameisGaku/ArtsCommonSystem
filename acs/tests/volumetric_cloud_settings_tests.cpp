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

ACS_TEST(VolumetricCloudSettings, MultipleScatteringAccumulatesThroughThirdOrderAndPreservesEnergyBound)
{
    /** 消散より大きい高次散乱係数を含む入力。 */
    FVolumetricCloudLighting requested{};
    requested.MultiScatterContribution = 0.8f;
    requested.MultiScatterOcclusion = 0.4f;
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

ACS_TEST(VolumetricCloudSettings, InScatterUsesLowLodDensityAndHeightWithoutAmplifyingLight)
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
    /** 同じ密度でも、散乱源が少ない雲底は暗くなる。 */
    const f32 denseBase = EvaluateVolumetricCloudInScatterFactor(1.0f, 0.0f, 1.0f);
    EXPECT_NEAR(denseBase, std::pow(0.10f, 0.8f), 1e-6f);
    EXPECT_TRUE(denseBase < denseTop);

    /** 旧実装の 0.70 の縁係数へ近い既定の移行結果。 */
    const FVolumetricCloudLighting defaults{};
    const f32 defaultSparseBase = EvaluateVolumetricCloudInScatterFactor(0.0f, 0.0f, defaults.PowderStrength);
    EXPECT_TRUE(defaultSparseBase >= 0.70f);
    EXPECT_TRUE(defaultSparseBase <= 0.71f);

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

    // Sceneが毎frame上書きする大気色も、環境cubemapの実際の照明入力なので追跡する。
    // 再生成頻度は固定frame間隔で別に制限される。
    lighting = clouds.Lighting();
    lighting.SkyZenithColor = FVec3{4.0f, 3.0f, 2.0f};
    lighting.SunTransmittance = FVec3{0.4f, 0.5f, 0.6f};
    clouds.SetLighting(lighting);
    EXPECT_TRUE(clouds.EnvironmentLightingSignature(0.55f, 1.6f, 1.0f) != lighting_changed);

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

    const std::string signature = SliceBetween(sky_source, "u32 CVolumetricClouds::EnvironmentLightingSignature(", "bool CVolumetricClouds::IsEnvironmentLightingRefreshFrame(");
    EXPECT_TRUE(Contains(signature, "m_PrevSunColor.x"));
    EXPECT_TRUE(Contains(signature, "m_PrevSkyColor.x"));
    EXPECT_TRUE(Contains(signature, "m_Lighting.SunTransmittance.x"));
    EXPECT_TRUE(Contains(signature, "m_Lighting.SkyZenithColor.x"));
    EXPECT_TRUE(Contains(signature, "HashCloudEnvironmentQuantizedFloat("));

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
    /** 最終 coverage から追加 fetch 無しで求める低 LOD 密度の実装範囲。 */
    const std::string lowLodDensity = SliceBetween(source, "float cloudLowLodDensityFromPositiveWeatherMacro(",
                                                   "float cloudDensityFromPositiveWeatherMacro(");
    EXPECT_TRUE(Contains(setLayer, "InvalidateCloudHistory_Internal(true);"));
    EXPECT_TRUE(Contains(setUpper, "InvalidateCloudHistory_Internal(true);"));
    EXPECT_TRUE(Contains(setReference, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(setLighting, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(setRange, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(header, "InvalidateCloudHistory_Internal(false);"));
    EXPECT_TRUE(Contains(source, "if (density_field_changed) m_ShadowCacheValid = false;"));
    EXPECT_TRUE(Contains(lowLodDensity, "macro.baseNoise,heightThreshold"));
    EXPECT_TRUE(Contains(lowLodDensity, "baseDensity*weatherMask*macro.profileClosure"));
    EXPECT_TRUE(Contains(source, "if(upperBand) weatherMask*=cloudUpperTerms.x;"));
    EXPECT_TRUE(Contains(source, "if(upperBand) densityResult*=cloudUpperTerms.y;"));
    EXPECT_FALSE(Contains(lowLodDensity, "SampleLevel"));

    EXPECT_TRUE(Contains(source, "cached.y*density*cloudLightingExtinction.y"));
    EXPECT_TRUE(Contains(source, "lightDepth*density*cloudLightingExtinction.y>18.0"));
    EXPECT_TRUE(Contains(source, "float inScatterDepthExponent=lerp("));
    EXPECT_TRUE(Contains(source, "float lowLodDensity=cloudLowLodDensityFromMacro("));
    EXPECT_TRUE(Contains(source, "0.05+pow(saturate(lowLodDensity),inScatterDepthExponent)"));
    EXPECT_TRUE(Contains(source, "float inScatterProbability=inScatterDepth*inScatterVertical;"));
    EXPECT_TRUE(Contains(source, "1.0,inScatterProbability,cloudLightingExtinction.w"));
    EXPECT_TRUE(Contains(source, "float singleScatter=beer*phase*inScatterFactor;"));
    EXPECT_TRUE(Contains(source, "float secondScatter=multiContribution"));
    EXPECT_TRUE(Contains(source, "float thirdContribution=multiContribution*multiContribution;"));
    EXPECT_TRUE(Contains(source, "float thirdOcclusion=multiOcclusion*multiOcclusion;"));
    EXPECT_TRUE(Contains(source, "float thirdScatter=thirdContribution"));
    EXPECT_TRUE(Contains(source, "float multipleScatter=secondScatter+thirdScatter;"));
    EXPECT_TRUE(Contains(source, "float scatterTerm=singleScatter+multipleScatter;"));
    EXPECT_FALSE(Contains(source, "nearLightDensity"));
    EXPECT_FALSE(Contains(source, "edgeBoost"));
    EXPECT_FALSE(Contains(source, "1.0-exp(-dens*cloudLightingExtinction.w)"));
    EXPECT_FALSE(Contains(source, "pow(saturate(shape),inScatterDepthExponent)"));
    EXPECT_FALSE(Contains(source, "beer*(1.0-multiWeight)*phase"));
    EXPECT_FALSE(Contains(source, "density*4.2"));
}

ACS_TEST(VolumetricCloudSettings, AmbientVisibilityUsesLocalIncomingOpticalDepth)
{
    /** GPU シェーダーを含む雲描画の実装。 */
    const std::string source = ReadRenderFile("Sky.cpp");
    /** 局所密度から空と地面の可視率を求める範囲。 */
    const std::string ambientBlock = SliceBetween(source, "float ambientLocalDensity=", "float a=1.0-exp(");
    EXPECT_TRUE(!ambientBlock.empty());
    EXPECT_TRUE(Contains(ambientBlock, "float ambientLocalDensity=saturate(lowLodDensity*density);"));
    EXPECT_TRUE(Contains(ambientBlock, "float skyAmbientOpticalDepth=ambientLocalDensity*(0.35+0.65*(1.0-h));"));
    EXPECT_TRUE(Contains(ambientBlock, "float groundAmbientOpticalDepth=ambientLocalDensity*(0.35+0.65*h);"));
    EXPECT_TRUE(Contains(ambientBlock, "float skyAmbientVisibility=exp(-0.60*skyAmbientOpticalDepth);"));
    EXPECT_TRUE(Contains(ambientBlock, "float groundAmbientVisibility=exp(-0.60*groundAmbientOpticalDepth);"));
    EXPECT_TRUE(Contains(ambientBlock, "*skyAmbientVisibility;"));
    EXPECT_TRUE(Contains(ambientBlock, "*bottomWeight*groundAmbientVisibility;"));
    EXPECT_FALSE(Contains(ambientBlock, "transmit"));
    EXPECT_FALSE(Contains(source, "float viewDepth=1.0-transmit;"));
    EXPECT_FALSE(Contains(source, "float ambientOcclusion="));

    /** 0 から 1 へ制限するシェーダー側 saturate の対応式。 */
    const auto saturate = [](f32 value) noexcept {
        if (value < 0.0f) return 0.0f;
        if (value > 1.0f) return 1.0f;
        return value;
    };
    /** 局所密度と高さから入射側の可視率を求める対応式。 */
    const auto visibility = [saturate](f32 lowLodDensity, f32 density, f32 height, bool fromSky) noexcept {
        /** 密度倍率を適用して 0 から 1 へ収めた局所密度。 */
        const f32 localDensity = saturate(lowLodDensity * density);
        /** 空は雲頂まで、地面反射は雲底までの距離を表す係数。 */
        const f32 boundaryDistance = fromSky ? 1.0f - height : height;
        /** 層境界で完全な 0 にせず、採取点周辺の厚みを残した光学的深さ。 */
        const f32 opticalDepth = localDensity * (0.35f + 0.65f * boundaryDistance);
        return std::exp(-0.60f * opticalDepth);
    };

    for (u32 densityStep = 0u; densityStep <= 4u; ++densityStep) {
        /** 検査対象の局所密度。 */
        const f32 density = static_cast<f32>(densityStep) * 0.25f;
        for (u32 heightStep = 0u; heightStep <= 100u; ++heightStep) {
            /** 雲底 0 から雲頂 1 までの高さ。 */
            const f32 height = static_cast<f32>(heightStep) * 0.01f;
            /** 雲頂側から届く空の可視率。 */
            const f32 skyVisibility = visibility(density, 1.0f, height, true);
            /** 雲底側から届く地面反射の可視率。 */
            const f32 groundVisibility = visibility(density, 1.0f, height, false);
            EXPECT_TRUE(std::isfinite(skyVisibility));
            EXPECT_TRUE(std::isfinite(groundVisibility));
            EXPECT_TRUE(skyVisibility > 0.0f && skyVisibility <= 1.0f);
            EXPECT_TRUE(groundVisibility > 0.0f && groundVisibility <= 1.0f);
            EXPECT_NEAR(skyVisibility, visibility(density, 1.0f, 1.0f - height, false), 1.0e-6f);
        }
    }

    EXPECT_TRUE(visibility(1.0f, 1.0f, 1.0f, true) > visibility(1.0f, 1.0f, 0.0f, true));
    EXPECT_TRUE(visibility(1.0f, 1.0f, 0.0f, false) > visibility(1.0f, 1.0f, 1.0f, false));
}
