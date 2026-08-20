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
std::string ReadRenderSource(const char* filename) {
    /** このテストソース自身の場所。 */
    const std::filesystem::path testFile{__FILE__};
    /** 読み込み対象となる描画実装ファイルの場所。 */
    const std::filesystem::path sourcePath = testFile.parent_path().parent_path() / "src" / "render" / filename;
    /** 描画実装を読み取る入力ストリーム。 */
    std::ifstream stream(sourcePath, std::ios::binary);
    if (!stream) stream.open(std::filesystem::path{"acs"} / "src" / "render" / filename, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/** 文字列が指定した断片を含むか返す。 */
bool Contains(const std::string& value, const char* fragment) {
    return value.find(fragment) != std::string::npos;
}

} // namespace

ACS_TEST(SkySunProfile, PhysicalDiscAndBoundedHaloUseExplicitAngularWidths) {
    constexpr f32 kRadiansToDegrees = 57.295779513f;
    /** 太陽円盤の1-cos値を角度へ戻した半径。 */
    const f32 discDegrees = std::acos(1.0f - kSkySolarDiscRadiusOneMinusCosine) * kRadiansToDegrees;
    /** 昼の光彩外端を角度へ戻した半径。 */
    const f32 dayHaloDegrees = std::acos(1.0f - kSkyDaySunHaloRadiusOneMinusCosine) * kRadiansToDegrees;
    /** 夕方の光彩外端を角度へ戻した半径。 */
    const f32 sunsetHaloDegrees = std::acos(1.0f - kSkySunsetSunHaloRadiusOneMinusCosine) * kRadiansToDegrees;
    EXPECT_NEAR(discDegrees, 0.2663f, 0.002f);
    EXPECT_TRUE(dayHaloDegrees > 4.9f && dayHaloDegrees < 5.1f);
    EXPECT_TRUE(sunsetHaloDegrees > 7.0f && sunsetHaloDegrees < 7.2f);

    /** 地平線上で太陽中心を見る放射成分。 */
    const FSkySunProfile center = ResolveSkySunProfile(0.0f, 0.0f, kSkySolarDiscRadiusOneMinusCosine, kSkyDaySunHaloRadiusOneMinusCosine, 0.0f);
    /** 円盤外側かつ昼の光彩内を見る放射成分。 */
    const FSkySunProfile halo = ResolveSkySunProfile(0.0005f, 0.0f, kSkySolarDiscRadiusOneMinusCosine, kSkyDaySunHaloRadiusOneMinusCosine, 0.0f);
    /** 昼の光彩外端より外を見る放射成分。 */
    const FSkySunProfile outside = ResolveSkySunProfile(kSkyDaySunHaloRadiusOneMinusCosine + 0.0001f, 0.0f, kSkySolarDiscRadiusOneMinusCosine, kSkyDaySunHaloRadiusOneMinusCosine, 0.0f);
    /** 太陽中心を見たまま地平線から上へ離れた放射成分。 */
    const FSkySunProfile high = ResolveSkySunProfile(0.0f, 0.5f, kSkySolarDiscRadiusOneMinusCosine, kSkyDaySunHaloRadiusOneMinusCosine, 0.0f);
    EXPECT_NEAR(center.disc_weight, 1.0f, 1.0e-6f);
    EXPECT_NEAR(center.halo_weight, 0.0f, 1.0e-6f);
    EXPECT_NEAR(center.horizon_glow_weight, kSkySunHorizonGlowStrength, 1.0e-6f);
    EXPECT_TRUE(halo.disc_weight == 0.0f);
    EXPECT_TRUE(halo.halo_weight > 0.0f && halo.halo_weight <= kSkySunHaloStrength);
    EXPECT_NEAR(outside.halo_weight, 0.0f, 1.0e-6f);
    EXPECT_TRUE(outside.horizon_glow_weight < 0.01f);
    EXPECT_TRUE(high.horizon_glow_weight < center.horizon_glow_weight * 0.01f);

    /** 不正入力を有限値へ直した放射成分。 */
    const FSkySunProfile sanitized = ResolveSkySunProfile(std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(), -std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity());
    EXPECT_TRUE(std::isfinite(sanitized.disc_weight));
    EXPECT_TRUE(std::isfinite(sanitized.halo_weight));
    EXPECT_TRUE(std::isfinite(sanitized.horizon_glow_weight));
}

ACS_TEST(SkySunProfile, PresetsAndPublicSettersPreserveValidAngularOrder) {
    /** 既定値と各プリセットを検査する空描画。 */
    CSky sky;
    EXPECT_NEAR(sky.SunRadius(), kSkySolarDiscRadiusOneMinusCosine, 1.0e-8f);
    EXPECT_NEAR(sky.SunGlow(), kSkyDaySunHaloRadiusOneMinusCosine, 1.0e-8f);
    sky.PresetSunset();
    EXPECT_NEAR(sky.SunRadius(), kSkySolarDiscRadiusOneMinusCosine, 1.0e-8f);
    EXPECT_NEAR(sky.SunGlow(), kSkySunsetSunHaloRadiusOneMinusCosine, 1.0e-8f);
    sky.PresetNight();
    EXPECT_NEAR(sky.SunGlow(), kSkyNightMoonHaloRadiusOneMinusCosine, 1.0e-8f);
    sky.PresetDay();
    EXPECT_NEAR(sky.SunGlow(), kSkyDaySunHaloRadiusOneMinusCosine, 1.0e-8f);

    sky.SetSunGlow(-1.0f);
    EXPECT_NEAR(sky.SunGlow(), sky.SunRadius(), 1.0e-8f);
    sky.SetSunRadius(0.5f);
    EXPECT_NEAR(sky.SunRadius(), 0.5f, 1.0e-8f);
    EXPECT_NEAR(sky.SunGlow(), 0.5f, 1.0e-8f);
    sky.SetSunGlow(4.0f);
    EXPECT_NEAR(sky.SunGlow(), 2.0f, 1.0e-8f);
    sky.SetSunRadius(std::numeric_limits<f32>::quiet_NaN());
    sky.SetSunGlow(std::numeric_limits<f32>::infinity());
    EXPECT_NEAR(sky.SunRadius(), 0.5f, 1.0e-8f);
    EXPECT_NEAR(sky.SunGlow(), 2.0f, 1.0e-8f);
}

ACS_TEST(SkySunProfile, FallbackAndCapturedEnvironmentUseTheSameBoundedProfile) {
    /** 画面へ直接描く空の実装ソース。 */
    const std::string skySource = ReadRenderSource("Sky.cpp");
    /** 間接光用に取得する空の実装ソース。 */
    const std::string iblSource = ReadRenderSource("Ibl.cpp");
    EXPECT_TRUE(!skySource.empty());
    EXPECT_TRUE(!iblSource.empty());
    for (/** 同じ太陽プロファイルを検査する実装ソース。 */ const std::string* source : {&skySource, &iblSource}) {
        EXPECT_TRUE(Contains(*source, "float angularAa = max(fwidth(sunAngle), 1.0e-7);"));
        EXPECT_TRUE(Contains(*source, "float haloWeight = haloProfile * haloProfile * 0.28 * (1.0 - discWeight);"));
        EXPECT_TRUE(Contains(*source, "float horizonBand = exp(-abs(t) * 12.0);"));
        EXPECT_TRUE(Contains(*source, "float forwardGlow = exp(-sunAngle / max(sun_params.y * 0.35, 1.0e-5)) * horizonBand * 0.18;"));
        EXPECT_TRUE(Contains(*source, "saturate(discWeight + haloWeight)"));
        EXPECT_TRUE(!Contains(*source, "pow(sun_d, 4.0)"));
        EXPECT_TRUE(!Contains(*source, "if (ang < sun_params.x)"));
    }
}
