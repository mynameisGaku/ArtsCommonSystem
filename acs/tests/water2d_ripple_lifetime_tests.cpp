// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Effects2D.h"

#include <cmath>
#include <limits>

using namespace acs;
using namespace acs::game;

ACS_TEST(Water2DRippleLifetime, ImpactPoolNeverOverwritesActiveRipples) {
    AWater2DComponent water;

    for (u32 i = 0; i < 16; ++i) {
        water.AddDisturbance(
            FVec2{static_cast<f32>(i), 0.0f}, 0.1f, 0.7f);
    }
    EXPECT_EQ(water.ActiveRippleCount(), 16u);

    water.OnUpdate(1.0f);
    water.AddDisturbance(FVec2{100.0f, 0.0f}, 0.1f, 0.7f);
    EXPECT_EQ(water.ActiveRippleCount(), 16u);

    // 満杯時に active 枠を上書きしていれば、1 秒遅れで追加した波紋だけが残る。
    water.OnUpdate(2.01f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water2DRippleLifetime, NegativeStrengthDecaysNaturally) {
    AWater2DComponent water;
    water.AddDisturbance(FVec2{0.0f, 0.0f}, 0.1f, -0.7f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);

    // Signed amplitude controls crest/trough phase; its sign must not end the
    // ripple on the first update.
    water.OnUpdate(0.01f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);

    water.OnUpdate(3.0f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water2DRippleLifetime, WakePoolCannotEraseImpactAndNeverOverwritesItself) {
    AWater2DComponent water;
    water.AddDisturbance(FVec2{0.0f, 0.0f}, 0.18f, 0.72f);

    for (u32 i = 0; i < 32; ++i) {
        water.AddWake(
            FVec2{static_cast<f32>(i), 0.0f},
            FVec2{4.0f, 0.0f}, 0.2f, 0.7f);
    }
    EXPECT_EQ(water.ActiveRippleCount(), 33u);

    water.OnUpdate(1.0f);
    for (u32 i = 0; i < 8; ++i) {
        water.AddWake(
            FVec2{100.0f + static_cast<f32>(i), 0.0f},
            FVec2{4.0f, 0.0f}, 0.2f, 0.7f);
    }
    EXPECT_EQ(water.ActiveRippleCount(), 33u);

    // 後発 wake が古い wake / impact を追い出していれば、ここで残存数が 0 にならない。
    water.OnUpdate(2.01f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water2DRippleLifetime, SmallRippleUsesContinuousLifetimeTail) {
    AWater2DComponent water;
    water.SetRippleDecay(1.0f, 0.0f);
    water.AddDisturbance(FVec2{0.0f, 0.0f}, 0.1f, 0.001f);

    // The removed absolute 0.002 cutoff used to discard this event on the
    // first update even though essentially its whole authored life remained.
    water.OnUpdate(0.01f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
    water.OnUpdate(0.989f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
    water.OnUpdate(0.002f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water2DRippleLifetime, LifetimeTailIsContinuousAndMonotonic) {
    constexpr f32 lifetime = 4.0f;
    f32 previous = 1.0f;
    for (u32 sample = 0; sample <= 400u; ++sample) {
        const f32 age = lifetime * static_cast<f32>(sample) / 400.0f;
        const f32 scale = AWater2DComponent::EvaluateRippleAmplitudeScale(
            age, lifetime, 0.0f);
        EXPECT_TRUE(std::isfinite(scale));
        EXPECT_TRUE(scale >= 0.0f);
        EXPECT_TRUE(scale <= previous + 1e-6f);
        previous = scale;
    }

    EXPECT_NEAR(AWater2DComponent::EvaluateRippleAmplitudeScale(
                    lifetime * 0.65f, lifetime, 0.0f),
                1.0f, 1e-6f);
    EXPECT_TRUE(AWater2DComponent::EvaluateRippleAmplitudeScale(
                    lifetime * 0.90f, lifetime, 0.0f) > 0.0f);
    EXPECT_TRUE(AWater2DComponent::EvaluateRippleAmplitudeScale(
                    lifetime * 0.999f, lifetime, 0.0f) < 1e-6f);
    EXPECT_NEAR(AWater2DComponent::EvaluateRippleAmplitudeScale(
                    lifetime, lifetime, 0.0f),
                0.0f, 1e-7f);
}

ACS_TEST(Water2DRippleLifetime, ExistingRippleKeepsCapturedDecaySettings) {
    AWater2DComponent water;
    water.SetRippleDecay(2.0f, 0.0f);
    water.AddDisturbance(FVec2{0.0f, 0.0f}, 0.1f, 0.5f);
    water.SetRippleDecay(0.1f, 64.0f);

    water.OnUpdate(0.2f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);
    water.OnUpdate(1.81f);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);
}

ACS_TEST(Water2DRippleLifetime, MalformedUpdatesAndEventsDoNotPoisonPool) {
    AWater2DComponent water;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();

    water.AddDisturbance(FVec2{nan, 0.0f}, 0.1f, 0.5f);
    water.AddDisturbance(FVec2{0.0f, 0.0f}, infinity, 0.5f);
    water.AddDisturbance(FVec2{0.0f, 0.0f}, 0.1f, nan);
    EXPECT_EQ(water.ActiveRippleCount(), 0u);

    water.AddDisturbance(FVec2{0.0f, 0.0f}, 0.1f, 0.5f);
    water.OnUpdate(nan);
    water.OnUpdate(-1.0f);
    EXPECT_EQ(water.ActiveRippleCount(), 1u);

    AWater2DComponent propagation_water;
    propagation_water.SetRipplePropagation(nan, infinity);
    propagation_water.AddDisturbance(
        FVec2{0.0f, 0.0f}, 0.1f, 0.5f);
    propagation_water.OnUpdate(0.1f);
    EXPECT_EQ(propagation_water.ActiveRippleCount(), 1u);
}
