// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/WeatherSystem.h"
#include "math/Math.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 浮動小数が非数や正負の無限大ではないか返す。 */
bool IsFiniteValue_Internal(f32 value) noexcept
{
    constexpr f32 kMaximumFiniteFloat = 3.402823466e+38f;
    return value == value && value >= -kMaximumFiniteFloat && value <= kMaximumFiniteFloat;
}

/** 3成分の近似一致を同じ許容誤差で検査する。 */
void ExpectVectorNear_Internal(FVec3 actual, FVec3 expected, f32 tolerance) noexcept
{
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
}

} // namespace

ACS_TEST(WeatherSystem, DefaultsToCompletedClearWeather)
{
    /** 初期状態を検査する天候システム。 */
    CWeatherSystem weather;
    EXPECT_EQ(weather.CurrentWeather(), EWeatherKind::Clear);
    EXPECT_EQ(weather.TargetWeather(), EWeatherKind::Clear);
    EXPECT_NEAR(weather.TransitionT(), 1.0f, 0.0f);
    EXPECT_NEAR(weather.AmbientLightMultiplier(), 1.0f, 0.0f);
    EXPECT_NEAR(weather.ParticleDensity(), 0.0f, 0.0f);
    ExpectVectorNear_Internal(weather.SkyTintMultiplier(), FVec3{1.0f, 1.0f, 1.0f}, 0.0f);
    EXPECT_NEAR(weather.WindStrength(), 0.1f, 0.0f);
    EXPECT_NEAR(weather.FogDensityMultiplier(), 1.0f, 0.0f);
}

ACS_TEST(WeatherSystem, TransitionUsesOneProgressValueForEveryOutput)
{
    /** 快晴から豪雨へ4秒で遷移する天候システム。 */
    CWeatherSystem weather;
    weather.SetWeather(EWeatherKind::HeavyRain, 4.0f);
    weather.Tick(2.0f);

    EXPECT_EQ(weather.CurrentWeather(), EWeatherKind::Clear);
    EXPECT_EQ(weather.TargetWeather(), EWeatherKind::HeavyRain);
    EXPECT_NEAR(weather.TransitionT(), 0.5f, 0.0f);
    EXPECT_NEAR(weather.AmbientLightMultiplier(), 0.775f, 1.0e-6f);
    EXPECT_NEAR(weather.ParticleDensity(), 1.0f, 1.0e-6f);
    ExpectVectorNear_Internal(weather.SkyTintMultiplier(), FVec3{0.775f, 0.8f, 0.85f}, 1.0e-6f);
    EXPECT_NEAR(weather.WindStrength(), 0.4f, 1.0e-6f);
    EXPECT_NEAR(weather.FogDensityMultiplier(), 1.4f, 1.0e-6f);
}

ACS_TEST(WeatherSystem, RetargetKeepsCurrentMixedOutputContinuous)
{
    /** 遷移途中で目標を変更する天候システム。 */
    CWeatherSystem weather;
    weather.SetWeather(EWeatherKind::HeavyRain, 4.0f);
    weather.Tick(2.0f);

    /** 目標変更直前の環境光倍率。 */
    const f32 ambientBefore = weather.AmbientLightMultiplier();
    /** 目標変更直前の粒子密度。 */
    const f32 particlesBefore = weather.ParticleDensity();
    /** 目標変更直前の空の色補正。 */
    const FVec3 skyBefore = weather.SkyTintMultiplier();
    /** 目標変更直前の風強さ。 */
    const f32 windBefore = weather.WindStrength();
    /** 目標変更直前の霧密度。 */
    const f32 fogBefore = weather.FogDensityMultiplier();

    weather.SetWeather(EWeatherKind::Cloudy, 2.0f);
    EXPECT_EQ(weather.CurrentWeather(), EWeatherKind::Clear);
    EXPECT_EQ(weather.TargetWeather(), EWeatherKind::Cloudy);
    EXPECT_NEAR(weather.TransitionT(), 0.0f, 0.0f);
    EXPECT_NEAR(weather.AmbientLightMultiplier(), ambientBefore, 0.0f);
    EXPECT_NEAR(weather.ParticleDensity(), particlesBefore, 0.0f);
    ExpectVectorNear_Internal(weather.SkyTintMultiplier(), skyBefore, 0.0f);
    EXPECT_NEAR(weather.WindStrength(), windBefore, 0.0f);
    EXPECT_NEAR(weather.FogDensityMultiplier(), fogBefore, 0.0f);

    weather.Tick(1.0f);
    EXPECT_NEAR(weather.AmbientLightMultiplier(), 0.8125f, 1.0e-6f);
    EXPECT_NEAR(weather.ParticleDensity(), 0.5f, 1.0e-6f);
    ExpectVectorNear_Internal(weather.SkyTintMultiplier(), FVec3{0.8125f, 0.84f, 0.885f}, 1.0e-6f);
    EXPECT_NEAR(weather.WindStrength(), 0.325f, 1.0e-6f);
    EXPECT_NEAR(weather.FogDensityMultiplier(), 1.3f, 1.0e-6f);
}

ACS_TEST(WeatherSystem, RepeatedTargetDoesNotRestartOrJumpTransition)
{
    /** 同じ目標が複数回通知される天候システム。 */
    CWeatherSystem weather;
    weather.SetWeather(EWeatherKind::Rain, 4.0f);
    weather.Tick(1.0f);
    /** 再指定前の環境光倍率。 */
    const f32 ambientBefore = weather.AmbientLightMultiplier();

    weather.SetWeather(EWeatherKind::Rain, 10.0f);
    EXPECT_NEAR(weather.TransitionT(), 0.25f, 0.0f);
    EXPECT_NEAR(weather.AmbientLightMultiplier(), ambientBefore, 0.0f);
    weather.Tick(1.0f);
    EXPECT_NEAR(weather.TransitionT(), 0.5f, 0.0f);

    weather.SetWeather(EWeatherKind::Rain, 0.0f);
    EXPECT_EQ(weather.CurrentWeather(), EWeatherKind::Rain);
    EXPECT_EQ(weather.TargetWeather(), EWeatherKind::Rain);
    EXPECT_NEAR(weather.TransitionT(), 1.0f, 0.0f);
    EXPECT_NEAR(weather.AmbientLightMultiplier(), 0.7f, 0.0f);
}

ACS_TEST(WeatherSystem, InvalidTimeAndKindCannotCreateNonFiniteState)
{
    /** 不正な時間と種別を入力する天候システム。 */
    CWeatherSystem weather;
    /** 平方根の定義域外入力から作る非数。 */
    const f32 notANumber = Sqrt(-1.0f);
    /** 浮動小数の表現範囲を越える指数から作る正の無限大。 */
    const f32 positiveInfinity = Exp(1000.0f);
    EXPECT_FALSE(IsFiniteValue_Internal(notANumber));
    EXPECT_FALSE(IsFiniteValue_Internal(positiveInfinity));

    weather.SetWeather(EWeatherKind::Storm, notANumber);
    EXPECT_EQ(weather.CurrentWeather(), EWeatherKind::Storm);
    EXPECT_NEAR(weather.TransitionT(), 1.0f, 0.0f);
    EXPECT_TRUE(IsFiniteValue_Internal(weather.AmbientLightMultiplier()));

    weather.SetWeather(EWeatherKind::Rain, positiveInfinity);
    EXPECT_EQ(weather.CurrentWeather(), EWeatherKind::Rain);
    EXPECT_NEAR(weather.TransitionT(), 1.0f, 0.0f);

    weather.SetWeather(EWeatherKind::Clear, 4.0f);
    weather.Tick(1.0f);
    /** 非数の経過時間を渡す直前の進行度。 */
    const f32 progressBefore = weather.TransitionT();
    /** 非数の経過時間を渡す直前の環境光倍率。 */
    const f32 ambientBefore = weather.AmbientLightMultiplier();
    weather.Tick(notANumber);
    EXPECT_NEAR(weather.TransitionT(), progressBefore, 0.0f);
    EXPECT_NEAR(weather.AmbientLightMultiplier(), ambientBefore, 0.0f);

    weather.Tick(positiveInfinity);
    EXPECT_EQ(weather.CurrentWeather(), EWeatherKind::Clear);
    EXPECT_NEAR(weather.TransitionT(), 1.0f, 0.0f);

    weather.SetWeather(static_cast<EWeatherKind>(255u), 0.0f);
    EXPECT_EQ(weather.CurrentWeather(), EWeatherKind::Clear);
    EXPECT_EQ(weather.TargetWeather(), EWeatherKind::Clear);
    EXPECT_NEAR(weather.TransitionT(), 1.0f, 0.0f);
}
